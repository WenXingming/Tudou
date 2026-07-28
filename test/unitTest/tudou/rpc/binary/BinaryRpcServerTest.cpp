// ============================================================================
// 验证 binary::Server 从网络帧到 Protobuf Service 再到响应帧的完整闭环。
// ============================================================================

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <memory>
#include <thread>

#include "binary_rpc.pb.h"
#include "test.pb.h"
#include "tudou/rpc/binary/FrameCodec.h"
#include "tudou/rpc/binary/Server.h"

namespace tudou {
namespace rpc {
namespace test {

namespace {

uint16_t reserve_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1
        || ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        ::close(fd);
        return 0;
    }

    ::close(fd);
    return ntohs(address.sin_port);
}

int connect_with_retry(uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        return -1;
    }

    for (int retry = 0; retry < 200; ++retry) {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (fd < 0) {
            return -1;
        }
        if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

void write_all(int fd, const std::string& bytes) {
    size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(
            fd,
            bytes.data() + written,
            bytes.size() - written);
        ASSERT_GT(count, 0);
        written += static_cast<size_t>(count);
    }
}

class EchoService : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        response->set_message("Echo: " + request->message());
        done->Run();
    }
};

} // namespace

class BinaryRpcServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = reserve_free_port();
        ASSERT_GT(port_, 0);

        server_ = std::make_unique<binary::Server>("127.0.0.1", port_, 0);
        server_->register_service(std::make_shared<EchoService>());
        serverThread_ = std::thread([this]() {
            server_->start();
        });
    }

    void TearDown() override {
        if (server_) {
            server_->stop();
        }
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
    }

    std::unique_ptr<binary::Server> server_;
    std::thread serverThread_;
    uint16_t port_ = 0;
};

TEST_F(BinaryRpcServerTest, ExecutesCompleteRequest) {
    const int fd = connect_with_retry(port_);
    ASSERT_GE(fd, 0);

    binary::CallHead head;
    head.set_service_name("tudou.rpc.test.TestEchoService");
    head.set_method_name("Echo");

    EchoRequest request;
    request.set_message("hello server");

    const binary::Frame requestFrame(
        binary::FrameType::Request,
        8888,
        head.SerializeAsString(),
        request.SerializeAsString());
    write_all(fd, binary::FrameCodec::encode(requestFrame));

    Buffer buffer;
    binary::Frame responseFrame;
    while (true) {
        char bytes[1024];
        const ssize_t count = ::read(fd, bytes, sizeof(bytes));
        ASSERT_GT(count, 0);
        buffer.write_to_buffer(bytes, static_cast<size_t>(count));

        const auto result = binary::FrameCodec::try_decode(buffer, responseFrame);
        ASSERT_NE(result, binary::FrameCodec::DecodeResult::Invalid);
        if (result == binary::FrameCodec::DecodeResult::Complete) {
            break;
        }
    }

    EXPECT_EQ(responseFrame.header.type, binary::FrameType::Response);
    EXPECT_EQ(responseFrame.header.sequenceId, 8888);

    EchoResponse response;
    ASSERT_TRUE(response.ParseFromString(responseFrame.body));
    EXPECT_EQ(response.message(), "Echo: hello server");
    ::close(fd);
}

} // namespace test
} // namespace rpc
} // namespace tudou
