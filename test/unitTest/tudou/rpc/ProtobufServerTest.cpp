/**
 * @file ProtobufServerTest.cpp
 * @brief 基于 Protobuf RPC 协议的二进制 TCP 服务端集成环回测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/protobuf/ProtobufServer.h"
#include "tudou/rpc/protocol/RpcCodec.h"
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
#include <cerrno>

namespace tudou {
namespace rpc {
namespace test {

namespace {

// 探测并预留一个空闲的 TCP 端口以进行测试绑定
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

// 客户端重试连接，以防后台 Reactor 服务线程尚未完成 listen 绑定
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
            return clientFd; // 连接成功
        }

        ::close(clientFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // 等待 5ms
    }

    return -1;
}

} // namespace

// 实现 Echo 业务
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

class ProtobufServerTest : public ::testing::Test {
protected:
    void SetUp() override {
        port = reserve_free_port();
        ASSERT_GT(port, 0);

        // 创建 RPC 服务端并监听分配好的确定端口
        server = std::make_unique<ProtobufServer>("127.0.0.1", port, 0);
        server->register_service(std::make_shared<TestEchoServiceImpl>());

        // 启动后台 Reactor 循环线程
        serverThread = std::thread([this]() {
            server->start();
        });
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

TEST_F(ProtobufServerTest, ExecutesLoopbackBinaryRpcSuccessfully) {
    // 1. 重试连接到服务端端口
    int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);

    // 2. 编码元信息 (Meta)
    RpcMeta meta;
    meta.set_service_name("tudou.rpc.test.TestEchoService");
    meta.set_method_name("Echo");
    std::string metaRaw;
    ASSERT_TRUE(meta.SerializeToString(&metaRaw));

    // 3. 编码业务请求体 (Body)
    EchoRequest request;
    request.set_message("Hello Tudou Binary RPC");
    std::string bodyRaw;
    ASSERT_TRUE(request.SerializeToString(&bodyRaw));

    // 4. 组装发送字节流
    Buffer writeBuf;
    RpcCodec::encode(&writeBuf, RpcMessageType::Request, 8888, metaRaw, bodyRaw);
    std::string bytesToSend = writeBuf.read_from_buffer();

    // 5. 通过 TCP 发送数据
    ssize_t written = ::write(clientFd, bytesToSend.data(), bytesToSend.size());
    ASSERT_EQ(written, static_cast<ssize_t>(bytesToSend.size()));

    // 6. 循环接收响应
    Buffer readBuf;
    char temp[1024];
    RpcHeader respHeader;
    std::string respMetaRaw;
    std::string respBodyRaw;

    while (true) {
        ssize_t nr = ::read(clientFd, temp, sizeof(temp));
        ASSERT_GT(nr, 0);
        readBuf.write_to_buffer(temp, nr);

        RpcCodec::DecodeResult decResult = RpcCodec::decode(&readBuf, respHeader, respMetaRaw, respBodyRaw);
        if (decResult == RpcCodec::DecodeResult::Success) {
            break;
        }
        ASSERT_NE(decResult, RpcCodec::DecodeResult::Error);
    }

    // 7. 验证返回数据正确性
    EXPECT_EQ(respHeader.sequenceId, 8888);
    EXPECT_EQ(static_cast<RpcMessageType>(respHeader.type), RpcMessageType::Response);

    EchoResponse response;
    ASSERT_TRUE(response.ParseFromString(respBodyRaw));
    EXPECT_EQ(response.message(), "Echo: Hello Tudou Binary RPC");

    ::close(clientFd);
}

} // namespace test
} // namespace rpc
} // namespace tudou
