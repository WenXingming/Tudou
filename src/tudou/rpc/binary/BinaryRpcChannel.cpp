/**
 * @file BinaryRpcChannel.cpp
 * @brief 基于二进制 RPC 协议支持单连接多路复用的客户端通道实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "BinaryRpcChannel.h"
#include "tudou/rpc/binary/BinaryRpcCodec.h"
#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"
#include "binary_rpc.pb.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace tudou {
namespace rpc {
namespace binary {

BinaryRpcChannel::BinaryRpcChannel(const std::string& ip, uint16_t port)
    : connected_(true) {
    clientFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd_ < 0) {
        throw std::runtime_error("BinaryRpcChannel: Failed to create socket");
    }

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    
    if (::inet_pton(AF_INET, ip.c_str(), &servAddr.sin_addr) != 1) {
        ::close(clientFd_);
        throw std::runtime_error("BinaryRpcChannel: Invalid IP address: " + ip);
    }

    if (::connect(clientFd_, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        ::close(clientFd_);
        throw std::runtime_error("BinaryRpcChannel: Failed to connect to server " + ip + ":" + std::to_string(port));
    }

    // 连接成功后，启动专职的后台接收解包线程
    receiverThread_ = std::thread([this]() {
        this->receive_loop();
    });
}

BinaryRpcChannel::BinaryRpcChannel(EventLoop* loop, const std::string& ip, uint16_t port)
    : loop_(loop), connected_(false) {
    
    // 1. 创建非阻塞 Socket，设置 SOCK_NONBLOCK
    clientFd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (clientFd_ < 0) {
        throw std::runtime_error("BinaryRpcChannel: Failed to create non-blocking socket");
    }

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    
    if (::inet_pton(AF_INET, ip.c_str(), &servAddr.sin_addr) != 1) {
        ::close(clientFd_);
        throw std::runtime_error("BinaryRpcChannel: Invalid IP address: " + ip);
    }

    // 2. 发起非阻塞 connect，如果返回小于 0，应为 EINPROGRESS 状态
    int ret = ::connect(clientFd_, (struct sockaddr*)&servAddr, sizeof(servAddr));
    if (ret < 0 && errno != EINPROGRESS) {
        ::close(clientFd_);
        throw std::runtime_error("BinaryRpcChannel: Failed to initiate non-blocking connect to " + ip + ":" + std::to_string(port));
    }

    // 3. 将 Socket 包装成 Channel 注册到 EventLoop 监听事件
    channel_ = std::make_unique<Channel>(loop_, clientFd_);
    channel_->set_read_callback([this](Channel&) { this->on_read(); });
    channel_->set_write_callback([this](Channel&) { this->on_write(); });
    
    // 监听读，以及用于确认 connect 是否完成的可写事件
    channel_->enable_reading();
    channel_->enable_writing();
}

BinaryRpcChannel::~BinaryRpcChannel() {
    running_ = false;
    
    if (channel_) {
        channel_->disable_all();
    }

    if (clientFd_ >= 0) {
        ::shutdown(clientFd_, SHUT_RDWR);
        ::close(clientFd_);
    }

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }

    // 清理析构时可能仍挂起未返回的请求
    cleanup_pending_requests("BinaryRpcChannel: Channel is being destructed");
}

void BinaryRpcChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                                 google::protobuf::RpcController* /* controller */,
                                 const google::protobuf::Message* request,
                                 google::protobuf::Message* response,
                                 google::protobuf::Closure* done) {
    uint64_t seq = nextSequenceId_++;
    auto context = std::make_shared<ResponseContext>();
    context->response = response;

    Coroutine* cur_coro = Coroutine::t_current_coroutine;

    if (cur_coro != nullptr && loop_ != nullptr) {
        // ───────────────── 【路径一：协程非阻塞模式】 ─────────────────
        context->coroutine = cur_coro->shared_from_this();
        {
            std::lock_guard<std::mutex> lock(mapMutex_);
            if (!running_) {
                throw std::runtime_error("BinaryRpcChannel: Channel is closed");
            }
            pendingRequests_[seq] = context;
        }

        std::string bytesToSend;
        try {
            bytesToSend = encode_request(method, request, seq);
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(mapMutex_);
            pendingRequests_.erase(seq);
            throw;
        }

        // 调用非阻塞发送管道
        write_request_nonblocking(bytesToSend);

        // 挂起当前协程，释放 CPU 执行权回退到 EventLoop 主循环
        cur_coro->yield();

        // 被唤醒恢复后，检查是否有异常缓存
        if (context->exception) {
            std::rethrow_exception(context->exception);
        }

        if (done) {
            done->Run();
        }
        return;
    }
    else {
        // ───────────────── 【路径二：传统线程阻塞模式】 ─────────────────
        std::future<void> future = context->promise.get_future();
        {
            std::lock_guard<std::mutex> lock(mapMutex_);
            if (!running_) {
                throw std::runtime_error("BinaryRpcChannel: Channel is closed");
            }
            pendingRequests_[seq] = context;
        }

        std::string bytesToSend;
        try {
            bytesToSend = encode_request(method, request, seq);
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(mapMutex_);
            pendingRequests_.erase(seq);
            throw;
        }

        try {
            std::lock_guard<std::mutex> lock(sendMutex_);
            size_t totalSent = 0;
            while (totalSent < bytesToSend.size()) {
                ssize_t n = ::write(clientFd_, bytesToSend.data() + totalSent, bytesToSend.size() - totalSent);
                if (n <= 0) {
                    if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error("BinaryRpcChannel: Failed to write bytes to socket");
                }
                totalSent += n;
            }
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(mapMutex_);
            pendingRequests_.erase(seq);
            throw;
        }

        // 阻塞当前操作系统线程，等待底层解包线程将其唤醒
        future.get();

        if (done) {
            done->Run();
        }
        return;
    }
}

void BinaryRpcChannel::receive_loop() {
    Buffer readBuf;
    char temp[1024];
    RpcHeader respHeader;
    std::string respMetaRaw;
    std::string respBodyRaw;

    while (running_) {
        ssize_t nr = ::read(clientFd_, temp, sizeof(temp));
        if (nr <= 0) {
            if (nr < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            // 对端断开或 Socket 被 shutdown/close 发生异常，退出接收循环并清理待处理事务
            cleanup_pending_requests("BinaryRpcChannel: Connection closed prematurely");
            break;
        }

        readBuf.write_to_buffer(temp, nr);

        while (running_) {
            BinaryRpcCodec::DecodeResult result = BinaryRpcCodec::decode(&readBuf, respHeader, respMetaRaw, respBodyRaw);
            
            if (result == BinaryRpcCodec::DecodeResult::Success) {
                std::shared_ptr<ResponseContext> context;
                
                // 加锁提取并从 Map 移走该 sequenceId，防止二次操作
                {
                    std::lock_guard<std::mutex> lock(mapMutex_);
                    auto it = pendingRequests_.find(respHeader.sequenceId);
                    if (it != pendingRequests_.end()) {
                        context = it->second;
                        pendingRequests_.erase(it);
                    }
                }

                if (context) {
                    // 反序列化并还原回出参 response
                    if (context->response->ParseFromString(respBodyRaw)) {
                        context->promise.set_value(); // 【核心唤醒】
                    } else {
                        context->promise.set_exception(std::make_exception_ptr(
                            std::runtime_error("BinaryRpcChannel: Failed to parse Response Message")
                        ));
                    }
                }
            }
            else if (result == BinaryRpcCodec::DecodeResult::HalfPack || result == BinaryRpcCodec::DecodeResult::Empty) {
                break; // 半包继续等待接收
            }
            else if (result == BinaryRpcCodec::DecodeResult::Error) {
                cleanup_pending_requests("BinaryRpcChannel: Detected binary protocol decode error");
                running_ = false;
                break;
            }
        }
    }
}

void BinaryRpcChannel::cleanup_pending_requests(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mapMutex_);
    auto err = std::make_exception_ptr(std::runtime_error(reason));
    for (auto& pair : pendingRequests_) {
        auto context = pair.second;
        if (context->coroutine) {
            context->exception = err;
            EventLoop* origin_loop = context->coroutine->get_loop();
            origin_loop->run_in_loop([coro = context->coroutine]() {
                coro->resume();
            });
        } else {
            context->promise.set_exception(err);
        }
    }
    pendingRequests_.clear();
}

void BinaryRpcChannel::on_read() {
    char temp[1024];
    bool socketErrorOrEOF = false;

    while (running_) {
        ssize_t nr = ::read(clientFd_, temp, sizeof(temp));
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 读空，等待下一次事件
            }
            socketErrorOrEOF = true;
            break;
        }
        if (nr == 0) {
            socketErrorOrEOF = true; // 对端关闭连接
            break;
        }
        readBuf_.write_to_buffer(temp, nr);
    }

    if (socketErrorOrEOF) {
        cleanup_pending_requests("BinaryRpcChannel: Connection closed or read error");
        running_ = false;
        if (channel_) {
            channel_->disable_all();
        }
        return;
    }

    // 解包并匹配分发
    RpcHeader respHeader;
    std::string respMetaRaw;
    std::string respBodyRaw;

    while (running_) {
        BinaryRpcCodec::DecodeResult result = BinaryRpcCodec::decode(&readBuf_, respHeader, respMetaRaw, respBodyRaw);
        
        if (result == BinaryRpcCodec::DecodeResult::Success) {
            std::shared_ptr<ResponseContext> context;
            {
                std::lock_guard<std::mutex> lock(mapMutex_);
                auto it = pendingRequests_.find(respHeader.sequenceId);
                if (it != pendingRequests_.end()) {
                    context = it->second;
                    pendingRequests_.erase(it);
                }
            }

            if (context) {
                if (context->response->ParseFromString(respBodyRaw)) {
                    if (context->coroutine) {
                        EventLoop* origin_loop = context->coroutine->get_loop();
                        origin_loop->run_in_loop([coro = context->coroutine]() {
                            coro->resume();
                        });
                    } else {
                        context->promise.set_value();
                    }
                } else {
                    auto err = std::make_exception_ptr(std::runtime_error("BinaryRpcChannel: Failed to parse Response Message"));
                    if (context->coroutine) {
                        context->exception = err;
                        EventLoop* origin_loop = context->coroutine->get_loop();
                        origin_loop->run_in_loop([coro = context->coroutine]() {
                            coro->resume();
                        });
                    } else {
                        context->promise.set_exception(err);
                    }
                }
            }
        }
        else if (result == BinaryRpcCodec::DecodeResult::HalfPack || result == BinaryRpcCodec::DecodeResult::Empty) {
            break;
        }
        else if (result == BinaryRpcCodec::DecodeResult::Error) {
            cleanup_pending_requests("BinaryRpcChannel: Protocol decode error");
            running_ = false;
            if (channel_) {
                channel_->disable_all();
            }
            break;
        }
    }
}

void BinaryRpcChannel::on_write() {
    // 检查非阻塞 Socket 连接状态（第一次可写时触发getsockopt检测连接是否成功）
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(clientFd_, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
        cleanup_pending_requests("BinaryRpcChannel: Non-blocking connect failed");
        running_ = false;
        if (channel_) {
            channel_->disable_all();
        }
        return;
    }

    // 发送缓冲区写出
    std::lock_guard<std::mutex> lock(sendMutex_);
    if (writeBuffer_.empty()) {
        channel_->disable_writing();
        return;
    }

    size_t totalSent = 0;
    while (totalSent < writeBuffer_.size()) {
        ssize_t n = ::write(clientFd_, writeBuffer_.data() + totalSent, writeBuffer_.size() - totalSent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                writeBuffer_ = writeBuffer_.substr(totalSent);
                return;
            }
            cleanup_pending_requests("BinaryRpcChannel: Non-blocking write error");
            running_ = false;
            if (channel_) {
                channel_->disable_all();
            }
            return;
        }
        totalSent += n;
    }

    writeBuffer_.clear();
    channel_->disable_writing();
}

void BinaryRpcChannel::write_request_nonblocking(const std::string& data) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    
    // 如果还未连接成功，或者发送缓存中已有挂起数据，则追加到缓存末尾等待 on_write 依次刷出
    if (!connected_ || !writeBuffer_.empty()) {
        writeBuffer_ += data;
        return;
    }

    size_t totalSent = 0;
    while (totalSent < data.size()) {
        ssize_t n = ::write(clientFd_, data.data() + totalSent, data.size() - totalSent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 写缓冲区满，记录剩余字节，并在 EventLoop 中开启 writable 监听以等待就绪通知
                writeBuffer_ += data.substr(totalSent);
                loop_->run_in_loop([this]() {
                    if (channel_) {
                        channel_->enable_writing();
                    }
                });
                break;
            }
            throw std::runtime_error("BinaryRpcChannel: Non-blocking socket write error");
        }
        totalSent += n;
    }
}

std::string BinaryRpcChannel::encode_request(const google::protobuf::MethodDescriptor* method,
                                             const google::protobuf::Message* request,
                                             uint64_t seq) {
    RpcMeta meta;
    meta.set_service_name(method->service()->full_name());
    meta.set_method_name(method->name());
    
    std::string metaRaw;
    if (!meta.SerializeToString(&metaRaw)) {
        throw std::runtime_error("BinaryRpcChannel: Failed to serialize RpcMeta");
    }

    std::string bodyRaw;
    if (!request->SerializeToString(&bodyRaw)) {
        throw std::runtime_error("BinaryRpcChannel: Failed to serialize Request Message");
    }

    Buffer writeBuf;
    BinaryRpcCodec::encode(&writeBuf, RpcMessageType::Request, seq, metaRaw, bodyRaw);
    return writeBuf.read_from_buffer();
}

} // namespace binary
} // namespace rpc
} // namespace tudou
