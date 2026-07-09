/**
 * @file BinaryRpcRouterTest.cpp
 * @brief 基于 Protobuf 运行时反射机制的动态 RPC 路由器单元测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/binary/BinaryRpcRouter.h"
#include "test.pb.h"

namespace tudou {
namespace rpc {
namespace binary {
namespace test {

namespace {
// 实现 proto 文件中声明的 TestEchoService 业务子类
class TestEchoServiceImpl : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController* controller,
              const EchoRequest* request,
              EchoResponse* response,
              google::protobuf::Closure* done) override {
        // 核心 Echo 业务逻辑
        response->set_message("Echo: " + request->message());
        if (done) {
            done->Run();
        }
    }
};
}

class BinaryRpcRouterTest : public ::testing::Test {
protected:
    void SetUp() override {
        router = std::make_unique<BinaryRpcRouter>();
        // 注册服务实例到动态路由表
        router->register_service(std::make_shared<TestEchoServiceImpl>());
    }

    std::unique_ptr<BinaryRpcRouter> router;
};

// 1. 验证常规合法的反射调用动态分发成功
TEST_F(BinaryRpcRouterTest, DispatchesValidCallSuccessfully) {
    EchoRequest request;
    request.set_message("Tudou RPC Works!");
    
    std::string requestRaw;
    ASSERT_TRUE(request.SerializeToString(&requestRaw));

    bool isCallbackCalled = false;
    std::string outResponseRaw;

    // 触发调用
    router->dispatch(
        "tudou.rpc.binary.test.TestEchoService", 
        "Echo", 
        requestRaw, 
        [&](const std::string& responseRaw) {
            isCallbackCalled = true;
            outResponseRaw = responseRaw;
        }
    );

    EXPECT_TRUE(isCallbackCalled);
    
    // 解析回包并断言
    EchoResponse response;
    ASSERT_TRUE(response.ParseFromString(outResponseRaw));
    EXPECT_EQ(response.message(), "Echo: Tudou RPC Works!");
}

// 2. 验证调用不存在的服务名时抛出对应异常
TEST_F(BinaryRpcRouterTest, ThrowsExceptionOnServiceNotFound) {
    std::string garbagePayload = "garbage";
    
    EXPECT_THROW({
        router->dispatch(
            "tudou.rpc.binary.test.MissingService", 
            "Echo", 
            garbagePayload, 
            [](const std::string&) {}
        );
    }, std::invalid_argument);
}

// 3. 验证调用不存在的方法名时抛出对应异常
TEST_F(BinaryRpcRouterTest, ThrowsExceptionOnMethodNotFound) {
    std::string garbagePayload = "garbage";
    
    EXPECT_THROW({
        router->dispatch(
            "tudou.rpc.binary.test.TestEchoService", 
            "NotExistMethod", 
            garbagePayload, 
            [](const std::string&) {}
        );
    }, std::invalid_argument);
}

// 4. 验证传入损坏的数据包载荷导致反序列化失败时抛出异常
TEST_F(BinaryRpcRouterTest, ThrowsExceptionOnPayloadParseFailure) {
    // 写入无法构成合法 EchoRequest 的异常流数据
    // Protobuf-3 对某些简单垃圾字节会解析为空（解析成功），但如果字节流包含无效格式的 wire_type，便会反序列化失败。
    // 在此传入绝对非法的字段二进制标签数据
    std::string corruptedPayload = "\xFF\xFF\xFF\xFF";

    EXPECT_THROW({
        router->dispatch(
            "tudou.rpc.binary.test.TestEchoService", 
            "Echo", 
            corruptedPayload, 
            [](const std::string&) {}
        );
    }, std::invalid_argument);
}

} // namespace test
} // namespace binary
} // namespace rpc
} // namespace tudou
