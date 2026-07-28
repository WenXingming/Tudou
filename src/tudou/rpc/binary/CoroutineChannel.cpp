// ============================================================================
// 协程 Channel 的主流程是：请求入写缓冲并 yield，EventLoop 收到响应后 resume。
// 所有成员都只在所属 EventLoop 线程访问，因此不需要发送锁或后台线程。
// ============================================================================

#include "tudou/rpc/binary/CoroutineChannel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cerrno>
#include <stdexcept>
#include <utility>

#include "binary_rpc.pb.h"
#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/rpc/binary/FrameCodec.h"
#include "tudou/rpc/binary/Coroutine.h"

namespace tudou {
namespace rpc {
namespace binary {

namespace {

ScopedFd create_nonblocking_socket() {
    ScopedFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw std::runtime_error("CoroutineChannel: Failed to create socket");
    }
    return socket;
}

} // namespace

CoroutineChannel::CoroutineChannel(EventLoop& loop, const std::string& ip, uint16_t port)
    : loop_(loop),
      socket_(create_nonblocking_socket()),
      channel_(&loop_, socket_.fd()),
      responseConnection_(),
      outputBuffer_(),
      pendingCalls_(),
      nextSequenceId_(1),
      connected_(false),
      open_(true) {
    assert(loop_.is_in_loop_thread());

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("CoroutineChannel: Invalid IP address: " + ip);
    }

    const int result = ::connect(socket_.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    if (result == 0) {
        connected_ = true;
    }
    else if (errno != EINPROGRESS) {
        throw std::runtime_error("CoroutineChannel: Failed to connect to server");
    }
    // EINPROGRESS 是非阻塞 connect 的正常结果；EPOLLOUT 到达后再用 SO_ERROR 判断是否建连成功。

    channel_.set_read_callback([this](::Channel&) {
        on_read();
    });
    channel_.set_write_callback([this](::Channel&) {
        on_write();
    });
    channel_.set_close_callback([this](::Channel&) {
        fail_pending_calls("CoroutineChannel: Connection closed");
    });
    channel_.set_error_callback([this](::Channel&) {
        fail_pending_calls("CoroutineChannel: Connection error");
    });

    channel_.enable_reading();
    if (!connected_) {
        channel_.enable_writing();
    }
}

CoroutineChannel::~CoroutineChannel() {
    assert(loop_.is_in_loop_thread());
    fail_pending_calls("CoroutineChannel: Channel closed");
}

void CoroutineChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                                  google::protobuf::RpcController*,
                                  const google::protobuf::Message* request,
                                  google::protobuf::Message* response,
                                  google::protobuf::Closure* done) {
    assert(loop_.is_in_loop_thread());

    Coroutine* coroutine = Coroutine::current();
    if (coroutine == nullptr || coroutine->get_loop() != &loop_) {
        throw std::logic_error("CoroutineChannel: CallMethod requires a coroutine on its EventLoop");
    }
    if (!open_) {
        throw std::runtime_error("CoroutineChannel: Connection is closed");
    }

    // 先登记再发送，避免响应先到却找不到 sequenceId。
    // coroutine 使保存 response 和 error 的协程栈保持存活。
    std::string error;
    const uint64_t sequenceId = nextSequenceId_++;
    pendingCalls_.emplace(
        sequenceId,
        PendingCall{response, coroutine->shared_from_this(), &error});

    try {
        outputBuffer_.write_to_buffer(encode_request(method, request, sequenceId));
        channel_.enable_writing();
    }
    catch (...) {
        // 请求还没有成功进入发送流程，撤销已经登记的 pending call。
        pendingCalls_.erase(sequenceId);
        throw;
    }

    // 只挂起当前协程；EventLoop 线程继续收发其他请求，响应到达后从这里继续。
    coroutine->yield();
    if (!error.empty()) {
        throw std::runtime_error(error);
    }
    if (done) {
        done->Run();
    }
}

void CoroutineChannel::on_read() {
    char bytes[64 * 1024];

    // 本轮持续读到 EAGAIN，及时排空内核接收缓冲区。
    while (open_) {
        const ssize_t count = ::read(socket_.fd(), bytes, sizeof(bytes));
        if (count > 0) {
            std::vector<Frame> frames;
            const std::string data(bytes, static_cast<size_t>(count));
            if (!responseConnection_.decode(data, frames)
                || !complete_responses(frames)) {
                fail_pending_calls("CoroutineChannel: Invalid response frame");
                return;
            }
            continue;
        }
        if (count == 0) {
            fail_pending_calls("CoroutineChannel: Connection closed");
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        fail_pending_calls("CoroutineChannel: Read failed");
        return;
    }
}

bool CoroutineChannel::complete_responses(const std::vector<Frame>& frames) {
    for (const auto& frame : frames) {
        if (frame.header.type != FrameType::Response) {
            return false;
        }

        const auto it = pendingCalls_.find(frame.header.sequenceId);
        if (it == pendingCalls_.end()) {
            return false;
        }

        PendingCall call = std::move(it->second);
        pendingCalls_.erase(it);

        if (!call.response->ParseFromString(frame.body)) {
            *call.error = "CoroutineChannel: Invalid response body";
        }
        // 延后恢复，避免从 Socket 读回调栈直接重入业务协程。
        loop_.queue_in_loop([coroutine = std::move(call.coroutine)]() {
            coroutine->resume();
        });
    }
    return true;
}

void CoroutineChannel::on_write() {
    if (!open_) {
        return;
    }

    if (!connected_) {
        int error = 0;
        socklen_t length = sizeof(error);
        // EPOLLOUT 只表示 connect 已结束，SO_ERROR 才给出真正的连接结果。
        if (::getsockopt(socket_.fd(), SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0) {
            fail_pending_calls("CoroutineChannel: Connect failed");
            return;
        }
        connected_ = true;
    }

    while (outputBuffer_.readable_bytes() > 0) {
        int savedErrno = 0;
        const ssize_t count = outputBuffer_.write_to_fd(socket_.fd(), savedErrno);
        if (count > 0) {
            continue;
        }
        if (count < 0 && savedErrno == EINTR) {
            continue;
        }
        if (count < 0 && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)) {
            return;
        }
        fail_pending_calls("CoroutineChannel: Write failed");
        return;
    }

    channel_.disable_writing();
}

std::string CoroutineChannel::encode_request(const google::protobuf::MethodDescriptor* method,
                                             const google::protobuf::Message* request,
                                             uint64_t sequenceId) {
    CallHead head;
    head.set_service_name(method->service()->full_name());
    head.set_method_name(method->name());

    const Frame frame(FrameType::Request, sequenceId, head.SerializeAsString(), request->SerializeAsString());
    return FrameCodec::encode(frame);
}

void CoroutineChannel::fail_pending_calls(const std::string& reason) {
    if (!open_) {
        return;
    }

    open_ = false;
    channel_.disable_all();

    // 所有等待者都必须恢复并观察到错误，否则协程会永久停在 yield()。
    for (auto& entry : pendingCalls_) {
        PendingCall& call = entry.second;
        *call.error = reason;
        loop_.queue_in_loop([coroutine = call.coroutine]() {
            coroutine->resume();
        });
    }
    pendingCalls_.clear();
}

} // namespace binary
} // namespace rpc
} // namespace tudou
