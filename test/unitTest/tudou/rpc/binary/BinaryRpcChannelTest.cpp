// ============================================================================
// 验证 CoroutineChannel 在 EventLoop 中完成非阻塞 RPC 调用。
// ============================================================================

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "test.pb.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/rpc/binary/Coroutine.h"
#include "tudou/rpc/binary/CoroutineChannel.h"
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

class BinaryRpcChannelTest : public ::testing::Test {
protected:
    void SetUp() override {
        port_ = reserve_free_port();
        ASSERT_GT(port_, 0);

        server_ = std::make_unique<binary::Server>("127.0.0.1", port_, 0);
        server_->register_service(std::make_shared<EchoService>());
        serverThread_ = std::thread([this]() {
            server_->start();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

TEST_F(BinaryRpcChannelTest, CoroutineChannelExecutesCall) {
    EventLoop loop;
    binary::CoroutineChannel channel(loop, "127.0.0.1", port_);

    bool completed = false;
    auto coroutine = std::make_shared<binary::Coroutine>(&loop, [&]() {
        TestEchoService_Stub stub(&channel);
        EchoRequest request;
        request.set_message("coroutine");
        EchoResponse response;
        stub.Echo(nullptr, &request, &response, nullptr);

        completed = response.message() == "Echo: coroutine";
        loop.quit();
    });

    coroutine->resume();
    loop.loop();
    EXPECT_TRUE(completed);
}

TEST_F(BinaryRpcChannelTest, CoroutineChannelMultiplexesCalls) {
    EventLoop loop;
    binary::CoroutineChannel channel(loop, "127.0.0.1", port_);

    constexpr int kCallCount = 10;
    int completed = 0;
    std::vector<std::shared_ptr<binary::Coroutine>> coroutines;
    for (int index = 0; index < kCallCount; ++index) {
        coroutines.push_back(std::make_shared<binary::Coroutine>(&loop, [&, index]() {
            TestEchoService_Stub stub(&channel);
            EchoRequest request;
            request.set_message("call_" + std::to_string(index));
            EchoResponse response;
            stub.Echo(nullptr, &request, &response, nullptr);

            if (response.message() == "Echo: call_" + std::to_string(index)) {
                ++completed;
            }
            if (completed == kCallCount) {
                loop.quit();
            }
        }));
    }

    for (const auto& coroutine : coroutines) {
        coroutine->resume();
    }
    loop.loop();
    EXPECT_EQ(completed, kCallCount);
}

TEST_F(BinaryRpcChannelTest, ReleasesPendingCoroutineWhenEventLoopIsDestroyed) {
    std::weak_ptr<binary::Coroutine> weakCoroutine;
    {
        EventLoop loop;
        {
            binary::CoroutineChannel channel(loop, "127.0.0.1", port_);
            auto coroutine = std::make_shared<binary::Coroutine>(&loop, [&]() {
                TestEchoService_Stub stub(&channel);
                EchoRequest request;
                EchoResponse response;
                stub.Echo(nullptr, &request, &response, nullptr);
            });

            weakCoroutine = coroutine;
            coroutine->resume();
            coroutine.reset();
        }
    }

    EXPECT_TRUE(weakCoroutine.expired());
}

TEST_F(BinaryRpcChannelTest, CoroutineChannelRequiresCoroutineContext) {
    EventLoop loop;
    binary::CoroutineChannel channel(loop, "127.0.0.1", port_);
    TestEchoService_Stub stub(&channel);
    EchoRequest request;
    EchoResponse response;

    EXPECT_THROW(
        stub.Echo(nullptr, &request, &response, nullptr),
        std::logic_error);
}

} // namespace test
} // namespace rpc
} // namespace tudou
