// ============================================================================
// Channel 的调用线程只负责发请求和等待，后台线程只负责收响应和匹配。
// ============================================================================

#include "tudou/rpc/binary/Channel.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include "binary_rpc.pb.h"
#include "tudou/rpc/binary/FrameCodec.h"

namespace tudou {
namespace rpc {
namespace binary {

namespace {

ScopedFd connect_blocking_socket(const std::string& ip, uint16_t port) {
    ScopedFd socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw std::runtime_error("Channel: Failed to create socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("Channel: Invalid IP address: " + ip);
    }

    if (::connect(socket.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        throw std::runtime_error("Channel: Failed to connect to server");
    }
    return socket;
}

} // namespace

Channel::Channel(const std::string& ip, uint16_t port)
    : socket_(connect_blocking_socket(ip, port)),
      multiplexer_(),
      responseConnection_(),
      receiverThread_(),
      sendMutex_() {
    receiverThread_ = std::thread([this]() {
        receive_loop();
    });
}

Channel::~Channel() {
    if (socket_.valid()) {
        ::shutdown(socket_.fd(), SHUT_RDWR);
    }
    fail_pending_calls("Channel: Channel closed");

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }
}

void Channel::CallMethod(const google::protobuf::MethodDescriptor* method,
                         google::protobuf::RpcController*,
                         const google::protobuf::Message* request,
                         google::protobuf::Message* response,
                         google::protobuf::Closure* done) {
    auto completion = std::make_shared<std::promise<void>>();
    std::future<void> future = completion->get_future();

    const uint64_t sequenceId = multiplexer_.register_call(
        [response, completion](const std::string& body, std::exception_ptr error) {
            if (error) {
                completion->set_exception(error);
                return;
            }
            if (!response->ParseFromString(body)) {
                completion->set_exception(std::make_exception_ptr(
                    std::runtime_error("Channel: Invalid response body")));
                return;
            }
            completion->set_value();
        });

    try {
        write_request(encode_request(method, request, sequenceId));
    }
    catch (...) {
        multiplexer_.cancel_call(sequenceId);
        throw;
    }

    future.get();
    if (done) {
        done->Run();
    }
}

void Channel::receive_loop() {
    char bytes[64 * 1024];

    while (multiplexer_.is_open()) {
        const ssize_t count = ::read(socket_.fd(), bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            fail_pending_calls("Channel: Connection closed");
            return;
        }

        std::vector<Frame> frames;
        const std::string data(bytes, static_cast<size_t>(count));
        if (!responseConnection_.decode(data, frames)
            || !complete_responses(frames)) {
            fail_pending_calls("Channel: Invalid response frame");
            return;
        }
    }
}

bool Channel::complete_responses(const std::vector<Frame>& frames) {
    for (const auto& frame : frames) {
        if (frame.header.type != FrameType::Response
            || !multiplexer_.complete_call(frame.header.sequenceId, frame.body)) {
            return false;
        }
    }
    return true;
}

void Channel::write_request(const std::string& bytes) {
    std::lock_guard<std::mutex> lock(sendMutex_);

    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(socket_.fd(), bytes.data() + written, bytes.size() - written);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Channel: Failed to send request");
        }
        written += static_cast<size_t>(count);
    }
}

std::string Channel::encode_request(const google::protobuf::MethodDescriptor* method,
                                    const google::protobuf::Message* request,
                                    uint64_t sequenceId) {
    CallHead head;
    head.set_service_name(method->service()->full_name());
    head.set_method_name(method->name());

    const Frame frame(FrameType::Request, sequenceId, head.SerializeAsString(), request->SerializeAsString());
    return FrameCodec::encode(frame);
}

void Channel::fail_pending_calls(const std::string& reason) {
    multiplexer_.close(std::make_exception_ptr(std::runtime_error(reason)));
}

} // namespace binary
} // namespace rpc
} // namespace tudou
