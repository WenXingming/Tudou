/**
 * @file ProtobufChannel.cpp
 * @brief 基于 Protobuf RPC 协议支持单连接多路复用的客户端通道实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "ProtobufChannel.h"
#include "tudou/rpc/protocol/RpcCodec.h"
#include "protocol.pb.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace tudou {
namespace rpc {

ProtobufChannel::ProtobufChannel(const std::string& ip, uint16_t port) {
    clientFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd_ < 0) {
        throw std::runtime_error("ProtobufChannel: Failed to create socket");
    }

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    
    if (::inet_pton(AF_INET, ip.c_str(), &servAddr.sin_addr) != 1) {
        ::close(clientFd_);
        throw std::runtime_error("ProtobufChannel: Invalid IP address: " + ip);
    }

    if (::connect(clientFd_, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        ::close(clientFd_);
        throw std::runtime_error("ProtobufChannel: Failed to connect to server " + ip + ":" + std::to_string(port));
    }

    // 连接成功后，启动专职的后台接收解包线程
    receiverThread_ = std::thread([this]() {
        this->receive_loop();
    });
}

ProtobufChannel::~ProtobufChannel() {
    running_ = false;
    
    if (clientFd_ >= 0) {
        // 先调用 shutdown 强行中断内核中可能仍阻塞在 read 上的接收线程
        ::shutdown(clientFd_, SHUT_RDWR);
        ::close(clientFd_);
    }

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }

    // 清理析构时可能仍挂起未返回的请求
    cleanup_pending_requests("ProtobufChannel: Channel is being destructed");
}

void ProtobufChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                                google::protobuf::RpcController* /* controller */,
                                const google::protobuf::Message* request,
                                google::protobuf::Message* response,
                                google::protobuf::Closure* done) {
    uint64_t seq = nextSequenceId_++;

    // 1. 创建同步唤醒的 Response 上下文，并注册存入全局映射表
    auto context = std::make_shared<ResponseContext>();
    context->response = response;
    std::future<void> future = context->promise.get_future();

    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        if (!running_) {
            throw std::runtime_error("ProtobufChannel: Channel is closed");
        }
        pendingRequests_[seq] = context;
    }

    // 2. 序列化 RPC 元信息包与请求 Message 体
    RpcMeta meta;
    meta.set_service_name(method->service()->full_name());
    meta.set_method_name(method->name());
    std::string metaRaw;
    if (!meta.SerializeToString(&metaRaw)) {
        std::lock_guard<std::mutex> lock(mapMutex_);
        pendingRequests_.erase(seq);
        throw std::runtime_error("ProtobufChannel: Failed to serialize RpcMeta");
    }

    std::string bodyRaw;
    if (!request->SerializeToString(&bodyRaw)) {
        std::lock_guard<std::mutex> lock(mapMutex_);
        pendingRequests_.erase(seq);
        throw std::runtime_error("ProtobufChannel: Failed to serialize Request Message");
    }

    // 3. 编码协议二进制帧
    Buffer writeBuf;
    RpcCodec::encode(&writeBuf, RpcMessageType::Request, seq, metaRaw, bodyRaw);
    std::string bytesToSend = writeBuf.read_from_buffer();

    // 4. 并发写入 Socket 缓存。必须加锁保护，防止多线程并发写入时发生数据流交错损坏
    try {
        std::lock_guard<std::mutex> lock(sendMutex_);
        size_t totalSent = 0;
        while (totalSent < bytesToSend.size()) {
            ssize_t n = ::write(clientFd_, bytesToSend.data() + totalSent, bytesToSend.size() - totalSent);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) {
                    continue;
                }
                throw std::runtime_error("ProtobufChannel: Failed to write bytes to socket");
            }
            totalSent += n;
        }
    }
    catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mapMutex_);
        pendingRequests_.erase(seq);
        throw;
    }

    // 5. 阻塞挂起当前业务线程，等待后台接收解包线程将其唤醒
    future.get();

    // 6. 执行业务完毕后的 Closure 回调
    if (done) {
        done->Run();
    }
}

void ProtobufChannel::receive_loop() {
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
            cleanup_pending_requests("ProtobufChannel: Connection closed prematurely");
            break;
        }

        readBuf.write_to_buffer(temp, nr);

        while (running_) {
            RpcCodec::DecodeResult result = RpcCodec::decode(&readBuf, respHeader, respMetaRaw, respBodyRaw);
            
            if (result == RpcCodec::DecodeResult::Success) {
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
                            std::runtime_error("ProtobufChannel: Failed to parse Response Message")
                        ));
                    }
                }
            }
            else if (result == RpcCodec::DecodeResult::HalfPack || result == RpcCodec::DecodeResult::Empty) {
                break; // 半包继续等待接收
            }
            else if (result == RpcCodec::DecodeResult::Error) {
                cleanup_pending_requests("ProtobufChannel: Detected binary protocol decode error");
                running_ = false;
                break;
            }
        }
    }
}

void ProtobufChannel::cleanup_pending_requests(const std::string& reason) {
    std::lock_guard<std::mutex> lock(mapMutex_);
    for (auto& pair : pendingRequests_) {
        // 向所有挂起线程注入 exception，防止调用线程发生无限死锁挂起
        pair.second->promise.set_exception(std::make_exception_ptr(
            std::runtime_error(reason)
        ));
    }
    pendingRequests_.clear();
}

} // namespace rpc
} // namespace tudou
