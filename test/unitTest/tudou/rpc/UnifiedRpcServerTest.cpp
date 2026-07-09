/**
 * @file UnifiedRpcServerTest.cpp
 * @brief 双轨统一 RPC 服务端单元/集成测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/UnifiedRpcServer.h"
#include "tudou/rpc/binary/BinaryRpcChannel.h"
#include "tudou/rpc/json/JsonRpcClient.h"
#include "binary_rpc.pb.h"
#include "test.pb.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <memory>

namespace tudou {
namespace rpc {
namespace binary {
namespace test {

namespace {
uint16_t reserve_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) return 0;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1
        || ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }
    ::close(fd);
    return ntohs(addr.sin_port);
}
}

namespace {
// 继承并实现 test.proto 定义的 TestEchoService
class TestEchoServiceImpl : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        response->set_message("UnifiedEcho: " + request->message());
        if (done) {
            done->Run();
        }
    }
};
}

class UnifiedRpcServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        binaryPort = reserve_free_port();
        jsonPort = reserve_free_port();
        ASSERT_GT(binaryPort, 0);
        ASSERT_GT(jsonPort, 0);

        server = std::make_unique<UnifiedRpcServer>("127.0.0.1", binaryPort, jsonPort, 0);
        server->register_service(std::make_shared<TestEchoServiceImpl>());

        serverThread = std::thread([this]() {
            server->start();
        });

        // 给予服务器 Reactor 足够的建连及初始化时间
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        server->stop();
        if (serverThread.joinable()) {
            serverThread.join();
        }
    }

    std::unique_ptr<UnifiedRpcServer> server;
    std::thread serverThread;
    uint16_t binaryPort = 0;
    uint16_t jsonPort = 0;
};

// 1. 验证统一服务端上的二进制 RPC 通道是否工作正常
TEST_F(UnifiedRpcServerTest, AccessesViaBinaryRpcChannel) {
    BinaryRpcChannel channel("127.0.0.1", binaryPort);
    TestEchoService_Stub stub(&channel);

    EchoRequest request;
    request.set_message("hello binary");
    EchoResponse response;

    stub.Echo(nullptr, &request, &response, nullptr);
    EXPECT_EQ(response.message(), "UnifiedEcho: hello binary");
}

// 2. 验证统一服务端上的 JSON-RPC 动态反射转换通道是否工作正常
TEST_F(UnifiedRpcServerTest, AccessesViaJsonRpcClient) {
    JsonRpcClient client("127.0.0.1", jsonPort);
    
    nlohmann::json params;
    params["message"] = "hello json";
    
    // 动态调用生成的反射方法名
    auto result = client.call("tudou.rpc.binary.test.TestEchoService.Echo", params);
    
    EXPECT_TRUE(result.contains("message"));
    EXPECT_EQ(result["message"].get<std::string>(), "UnifiedEcho: hello json");
}

} // namespace test
} // namespace binary
} // namespace rpc
} // namespace tudou
