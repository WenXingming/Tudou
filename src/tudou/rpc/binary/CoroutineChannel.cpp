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
    : socket_(create_nonblocking_socket()),
      loop_(loop),
      channel_(nullptr),
      multiplexer_(),
      responseConnection_(),
      outputBuffer_(),
      connected_(false) {
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

    channel_ = std::make_unique<::Channel>(&loop_, socket_.fd());
    channel_->set_read_callback([this](::Channel&) {
        on_read();
    });
    channel_->set_write_callback([this](::Channel&) {
        on_write();
    });
    channel_->set_close_callback([this](::Channel&) {
        fail_pending_calls("CoroutineChannel: Connection closed");
    });
    channel_->set_error_callback([this](::Channel&) {
        fail_pending_calls("CoroutineChannel: Connection error");
    });

    channel_->enable_reading();
    if (!connected_) {
        channel_->enable_writing();
    }
}

CoroutineChannel::~CoroutineChannel() {
    assert(loop_.is_in_loop_thread());

    channel_->disable_all();
    channel_.reset();
    if (socket_.valid()) {
        ::shutdown(socket_.fd(), SHUT_RDWR);
    }
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

    auto callResult = std::make_shared<CallResult>();
    auto coroutineOwner = coroutine->shared_from_this();
    const uint64_t sequenceId = multiplexer_.register_call(
        [response, callResult, coroutineOwner](const std::string& body, std::exception_ptr error) {
            callResult->error = error;
            if (!callResult->error && !response->ParseFromString(body)) {
                callResult->error = std::make_exception_ptr(std::runtime_error(
                    "CoroutineChannel: Invalid response body"));
            }

            coroutineOwner->get_loop()->queue_in_loop([coroutineOwner]() {
                coroutineOwner->resume();
            });
        });

    try {
        queue_request(encode_request(method, request, sequenceId));
    }
    catch (...) {
        multiplexer_.cancel_call(sequenceId);
        throw;
    }

    coroutine->yield();
    if (callResult->error) {
        std::rethrow_exception(callResult->error);
    }
    if (done) {
        done->Run();
    }
}

void CoroutineChannel::on_read() {
    char bytes[64 * 1024];

    while (multiplexer_.is_open()) {
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

void CoroutineChannel::on_write() {
    if (!multiplexer_.is_open()) {
        return;
    }

    if (!connected_) {
        int error = 0;
        socklen_t length = sizeof(error);
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
        if (count < 0
            && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)) {
            return;
        }
        fail_pending_calls("CoroutineChannel: Write failed");
        return;
    }

    channel_->disable_writing();
}

bool CoroutineChannel::complete_responses(const std::vector<Frame>& frames) {
    for (const auto& frame : frames) {
        if (frame.header.type != FrameType::Response
            || !multiplexer_.complete_call(frame.header.sequenceId, frame.body)) {
            return false;
        }
    }
    return true;
}

void CoroutineChannel::queue_request(const std::string& bytes) {
    outputBuffer_.write_to_buffer(bytes);
    channel_->enable_writing();
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
    if (channel_) {
        channel_->disable_all();
    }
    multiplexer_.close(std::make_exception_ptr(std::runtime_error(reason)));
}

} // namespace binary
} // namespace rpc
} // namespace tudou
