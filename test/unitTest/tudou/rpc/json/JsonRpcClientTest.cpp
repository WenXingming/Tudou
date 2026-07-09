/**
 * @file JsonRpcClientTest.cpp
 * @brief JSON-RPC 2.0 C++ 客户端与服务端闭环集成测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/json/JsonRpcServer.h"
#include "tudou/rpc/json/JsonRpcClient.h"

#include <thread>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace tudou {
namespace rpc {
namespace test {

namespace {

// 预留端口
uint16_t reserve_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }

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

} // namespace

class JsonRpcClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        port = reserve_free_port();
        ASSERT_GT(port, 0);

        server = std::make_unique<JsonRpcServer>("127.0.0.1", port, 0);
        
        // 注册业务方法 add: 计算数组两个数之和
        server->register_method("add", [](const nlohmann::json& params) {
            if (!params.is_array() || params.size() != 2) {
                throw std::invalid_argument("params must be array of 2 numbers");
            }
            return params[0].get<int>() + params[1].get<int>();
        });

        // 后台启动服务器
        serverThread = std::thread([this]() {
            server->start();
        });

        // 稍微等待 Reactor 运行
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    void TearDown() override {
        server->stop();
        if (serverThread.joinable()) {
            serverThread.join();
        }
    }

    std::unique_ptr<JsonRpcServer> server;
    std::thread serverThread;
    uint16_t port = 0;
};

// 1. 测试合法的远程调用成功返回结果
TEST_F(JsonRpcClientTest, InvokesValidRpcMethodSuccessfully) {
    // 实例化客户端，内部会自动同步 connect
    JsonRpcClient client("127.0.0.1", port);

    nlohmann::json params = nlohmann::json::array({15, 25});
    nlohmann::json result = client.call("add", params);

    EXPECT_EQ(result.get<int>(), 40);
}

// 2. 测试调用不存在的方法时，客户端捕获并抛出 std::runtime_error 异常
TEST_F(JsonRpcClientTest, ThrowsExceptionOnMethodNotFound) {
    JsonRpcClient client("127.0.0.1", port);

    EXPECT_THROW({
        client.call("missing_method", nullptr);
    }, std::runtime_error);
}

// 3. 测试传入非法参数造成业务侧抛异常时，客户端能够抛出 std::runtime_error
TEST_F(JsonRpcClientTest, ThrowsExceptionOnInvalidParams) {
    JsonRpcClient client("127.0.0.1", port);
    
    // add 方法期望 params 是个 array，此处传入 string 触发 Server 端的-32602错误
    nlohmann::json badParams = "invalid_param_type";

    EXPECT_THROW({
        client.call("add", badParams);
    }, std::runtime_error);
}

} // namespace test
} // namespace rpc
} // namespace tudou
