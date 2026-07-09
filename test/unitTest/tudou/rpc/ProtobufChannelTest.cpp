/**
 * @file ProtobufChannelTest.cpp
 * @brief 基于 Protobuf RPC 协议支持单连接多路复用的客户端通道集成测试
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
#include <vector>
#include <stdexcept>
#include <atomic>

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

// 实现业务逻辑，增加了延时方法以模拟长耗时 RPC 任务
class TestEchoServiceImpl : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        
        // 如果输入数据包含 "slow_call"，则故意延迟处理以测试中途断开连接
        if (request->message().find("slow_call") != std::string::npos) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

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

        server = std::make_unique<ProtobufServer>("127.0.0.1", port, 0);
        server->register_service(std::make_shared<TestEchoServiceImpl>());

        serverThread = std::thread([this]() {
            server->start();
        });

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

// 1. 验证常规的单路调用成功
TEST_F(ProtobufChannelTest, InvokesSingleCallSuccessfully) {
    ProtobufChannel channel("127.0.0.1", port);
    TestEchoService_Stub stub(&channel);

    EchoRequest request;
    request.set_message("Single RPC verification");
    EchoResponse response;

    stub.Echo(nullptr, &request, &response, nullptr);
    EXPECT_EQ(response.message(), "Echo: Single RPC verification");
}

// 2. 【核心多路复用】验证多线程在单连接上发起并发调用时，回包内容精准匹配不发生混淆或交错
TEST_F(ProtobufChannelTest, ExecutesConcurrentMultiplexedCallsSuccessfully) {
    ProtobufChannel channel("127.0.0.1", port);
    TestEchoService_Stub stub(&channel);

    constexpr int kThreadNum = 10;
    std::vector<std::thread> workers;
    workers.reserve(kThreadNum);

    std::atomic<int> successCount{0};

    for (int index = 0; index < kThreadNum; ++index) {
        workers.emplace_back([&stub, index, &successCount]() {
            EchoRequest req;
            req.set_message("msg_" + std::to_string(index));
            EchoResponse resp;

            // 并发发起调用，所有线程均在后台同一 RpcChannel 上跑 write/promise/read 流程
            stub.Echo(nullptr, &req, &resp, nullptr);

            if (resp.message() == "Echo: msg_" + std::to_string(index)) {
                successCount++;
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    // 断言所有并发调用的返回全部精准无差错还原
    EXPECT_EQ(successCount.load(), kThreadNum);
}

// 3. 【核心容错逻辑】验证在长耗时 RPC 任务执行中途，Channel 被主动析构时，挂起线程能被正确唤醒并抛出异常
TEST_F(ProtobufChannelTest, ThrowsExceptionOnPrematureChannelDestruction) {
    auto channel = std::make_unique<ProtobufChannel>("127.0.0.1", port);
    TestEchoService_Stub stub(channel.get());

    EchoRequest req;
    req.set_message("slow_call_test");
    EchoResponse resp;

    // 启动一个后台线程执行 slow 任务调用
    std::thread callerThread([&stub, &req, &resp]() {
        EXPECT_THROW({
            stub.Echo(nullptr, &req, &resp, nullptr);
        }, std::runtime_error);
    });

    // 稍微等待调用发出，并正在服务端 sleep 时
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 主动析构客户端 Channel，模拟连接超时被清理或通道生命周期结束
    channel.reset();

    // 阻塞的 callerThread 应当由于析构中的 cleanup_pending_requests 写入异常而立即被唤醒，不会永远卡死！
    callerThread.join();
}

} // namespace test
} // namespace rpc
} // namespace tudou
