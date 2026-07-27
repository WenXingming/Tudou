/**
 * @file JsonRpcServerTest.cpp
 * @brief 基于 TCP 传输的 JSON-RPC 2.0 服务端单元测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <memory>
#include <thread>
#include <chrono>
#include "tudou/rpc/JsonRpcServer.h"

namespace {

// 辅助函数：预先筛选挑选一个系统空闲可用的 TCP 端口
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

// 辅助函数：建立客户端连接并包含高容错重试，避免多线程启动阶段的连接竞态拒绝
int connect_with_retry(uint16_t port) {
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) != 1) {
        return -1;
    }

    for (int retry = 0; retry < 200; ++retry) {
        const int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (clientFd < 0) {
            return -1;
        }

        if (::connect(clientFd, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) == 0) {
            return clientFd;
        }

        ::close(clientFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return -1;
}

// 辅助函数：从客户端 socket 中读取数据，直至遇到换行符 '\n' 结束标记
std::string read_line(int fd) {
    std::string line;
    char c = 0;
    while (true) {
        ssize_t n = ::read(fd, &c, 1);
        if (n <= 0) {
            break; // 错误或连接关闭
        }
        line.push_back(c);
        if (c == '\n') {
            break;
        }
    }
    return line;
}

// 辅助函数：将完整数据写入 socket
void write_all(int fd, const std::string& data) {
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(fd, data.data() + total, data.size() - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
}

} // namespace

class JsonRpcServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 1. 自动筛选分配一个安全的空闲端口
        port = reserve_free_port();
        ASSERT_NE(port, 0);

        // 2. 实例化 RPC Server 并注册业务逻辑方法
        server = std::make_unique<JsonRpcServer>("127.0.0.1", port);
        
        server->register_method("add", [](const nlohmann::json& params) {
            return params[0].get<int>() + params[1].get<int>();
        });
        
        server->register_method("greet", [](const nlohmann::json& params) {
            if (params.is_array() && !params.empty()) {
                return "Hello, " + params[0].get<std::string>() + "!";
            }
            return "Hello, " + params.get<std::string>() + "!";
        });

        // 3. 将服务器 start 的阻塞监听放到独立的后台线程执行
        serverThread = std::thread([this]() {
            server->start();
        });
    }

    void TearDown() override {
        // 服务端主动停止，这会退出主 loop()
        server->stop();
        // 等待后台运行线程 join 回调释放资源
        if (serverThread.joinable()) {
            serverThread.join();
        }
        server.reset();
    }

    uint16_t port = 0;
    std::unique_ptr<JsonRpcServer> server;
    std::thread serverThread;
};

// 1. 验证常规同步请求与响应的 TCP 交互
TEST_F(JsonRpcServerTest, ProcessesSingleRpcRequestOverTcp) {
    int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);

    // 发送以 \n 结尾的请求
    std::string request = R"({"jsonrpc": "2.0", "method": "add", "params": [10, 25], "id": 999})" "\n";
    write_all(clientFd, request);

    // 读取响应行并验证其格式与计算正确性
    std::string responseLine = read_line(clientFd);
    ASSERT_FALSE(responseLine.empty());
    EXPECT_EQ(responseLine.back(), '\n');

    // 剥离换行符后反序列化
    responseLine.pop_back();
    nlohmann::json resp = nlohmann::json::parse(responseLine);

    EXPECT_EQ(resp["jsonrpc"], "2.0");
    EXPECT_EQ(resp["result"], 35);
    EXPECT_EQ(resp["id"], 999);
    EXPECT_FALSE(resp.contains("error"));

    ::close(clientFd);
}

// 2. 验证多路复用下的批量请求交互
TEST_F(JsonRpcServerTest, ProcessesBatchRpcRequestsOverTcp) {
    int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);

    // 发送 Batch 数组 (紧凑单行格式，且 params 以数组包装符合 JSON-RPC 规范)
    std::string request = "[{\"jsonrpc\": \"2.0\", \"method\": \"greet\", \"params\": [\"Tudou\"], \"id\": 1001},"
                          "{\"jsonrpc\": \"2.0\", \"method\": \"add\", \"params\": [100, 200], \"id\": 1002}]\n";
    write_all(clientFd, request);

    std::string responseLine = read_line(clientFd);
    ASSERT_FALSE(responseLine.empty());
    EXPECT_EQ(responseLine.back(), '\n');

    responseLine.pop_back();
    nlohmann::json resp = nlohmann::json::parse(responseLine);

    ASSERT_TRUE(resp.is_array());
    ASSERT_EQ(resp.size(), 2);

    EXPECT_EQ(resp[0]["id"], 1001);
    EXPECT_EQ(resp[0]["result"], "Hello, Tudou!");

    EXPECT_EQ(resp[1]["id"], 1002);
    EXPECT_EQ(resp[1]["result"], 300);

    ::close(clientFd);
}
