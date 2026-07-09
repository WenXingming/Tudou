/**
 * @file ProtobufChannelTest.cpp
 * @brief 基于 Protobuf RPC 协议的二进制客户端传输通道集成测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/protobuf/ProtobufServer.h"
#include "tudou/rpc/protobuf/ProtobufChannel.h"
#include "protocol.pb.h"
#include "test.pb.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <memory>

namespace tudou {
namespace rpc {
namespace test {

namespace {

// 探测并预留空闲测试端口
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

// 实现 Echo 业务逻辑
class TestEchoServiceImpl : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        response->set_message("Echo: " + request->message());
        if (done) {
            done->Run();
        }
    }
};

class ProtobufChannelTest : public ::testing::Test {
protected:
    void SetUp() override {
        port = reserve_free_port();
        ASSERT_GT(port, 0);

        // 创建 RPC 服务端并监听分配好的端口
        server = std::make_unique<ProtobufServer>("127.0.0.1", port, 0);
        server->register_service(std::make_shared<TestEchoServiceImpl>());

        // 启动后台线程运行服务器
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

    std::unique_ptr<ProtobufServer> server;
    std::thread serverThread;
    uint16_t port = 0;
};

// 验证客户端使用生成的 Stub 发起强类型 RPC 请求能够顺利完成环回
TEST_F(ProtobufChannelTest, InvokesStubCallSuccessfully) {
    // 实例化物理通信管道（内部自动发起 TCP connect）
    ProtobufChannel channel("127.0.0.1", port);

    // 实例化 protoc 编译器生成的 Service 客户端代理 Stub
    TestEchoService_Stub stub(&channel);

    EchoRequest request;
    request.set_message("Strong-typed RPC Verification!");

    EchoResponse response;

    // 发起强类型同步调用！此过程会将参数通过 channel 打包网络传输并等待解包回填
    // 按照用户指示，暂不传递 RpcController (传入 nullptr)
    stub.Echo(nullptr, &request, &response, nullptr);

    // 断言返回值完全一致
    EXPECT_EQ(response.message(), "Echo: Strong-typed RPC Verification!");
}

} // namespace test
} // namespace rpc
} // namespace tudou
