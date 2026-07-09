/**
 * @file JsonRpcServiceTest.cpp
 * @brief JSON-RPC 2.0 协议处理器单元测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/JsonRpcService.h"

class JsonRpcServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 注册测试函数 add: 接收包含两个数字的数组，返回它们的和
        service.register_method("add", [](const nlohmann::json& params) {
            if (!params.is_array() || params.size() != 2) {
                throw std::invalid_argument("params must be an array of size 2");
            }
            return params[0].get<int>() + params[1].get<int>();
        });

        // 注册测试函数 echo: 直接返回收到的参数
        service.register_method("echo", [](const nlohmann::json& params) {
            return params;
        });

        // 注册测试通知函数 notify_test: 改变外部标志位，无返回值
        service.register_method("notify_test", [this](const nlohmann::json&) {
            notificationTriggered = true;
            return nullptr;
        });
    }

    JsonRpcService service;
    bool notificationTriggered = false;
};

// 1. 测试合法请求的执行
TEST_F(JsonRpcServiceTest, ProcessesValidRequestSuccessfully) {
    std::string request = R"({"jsonrpc": "2.0", "method": "add", "params": [3, 4], "id": 1})";
    std::string responseStr = service.dispatch(request);
    
    ASSERT_FALSE(responseStr.empty());
    nlohmann::json response = nlohmann::json::parse(responseStr);
    
    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["result"], 7);
    EXPECT_EQ(response["id"], 1);
    EXPECT_FALSE(response.contains("error"));
}

// 2. 测试参数无效（-32602）错误捕获
TEST_F(JsonRpcServiceTest, HandlesInvalidParamsException) {
    // 传入非数组的 params 参数，add 处理器会抛出 std::invalid_argument
    std::string request = R"({"jsonrpc": "2.0", "method": "add", "params": "not_an_array", "id": 2})";
    std::string responseStr = service.dispatch(request);
    
    ASSERT_FALSE(responseStr.empty());
    nlohmann::json response = nlohmann::json::parse(responseStr);
    
    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["id"], 2);
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32602);
}

// 3. 测试方法未找到（-32601）错误捕获
TEST_F(JsonRpcServiceTest, HandlesMethodNotFound) {
    std::string request = R"({"jsonrpc": "2.0", "method": "missing_method", "params": [], "id": 3})";
    std::string responseStr = service.dispatch(request);
    
    ASSERT_FALSE(responseStr.empty());
    nlohmann::json response = nlohmann::json::parse(responseStr);
    
    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["id"], 3);
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32601);
}

// 4. 测试语法解析失败（-32700）错误捕获
TEST_F(JsonRpcServiceTest, HandlesJsonParseError) {
    std::string request = R"({"jsonrpc": "2.0", "method": "add", "params": [1, 2)"; // 格式损坏的 JSON
    std::string responseStr = service.dispatch(request);
    
    ASSERT_FALSE(responseStr.empty());
    nlohmann::json response = nlohmann::json::parse(responseStr);
    
    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_TRUE(response["id"].is_null());
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32700);
}

// 5. 测试协议格式不合规（-32600）错误捕获
TEST_F(JsonRpcServiceTest, HandlesInvalidRequestStruct) {
    // 缺失 jsonrpc 版本声明
    std::string request = R"({"method": "add", "params": [1, 2], "id": 5})";
    std::string responseStr = service.dispatch(request);
    
    ASSERT_FALSE(responseStr.empty());
    nlohmann::json response = nlohmann::json::parse(responseStr);
    
    EXPECT_EQ(response["jsonrpc"], "2.0");
    EXPECT_EQ(response["id"], 5);
    ASSERT_TRUE(response.contains("error"));
    EXPECT_EQ(response["error"]["code"], -32600);
}

// 6. 测试 Notification (通知) 机制，即不包含 id 属性，无需回复且正常执行逻辑
TEST_F(JsonRpcServiceTest, NotificationExecutesWithoutResponse) {
    std::string request = R"({"jsonrpc": "2.0", "method": "notify_test"})";
    EXPECT_FALSE(notificationTriggered);

    std::string responseStr = service.dispatch(request);
    
    // 规范要求：通知不得产生任何响应文本
    EXPECT_TRUE(responseStr.empty());
    // 断言业务处理器成功被调用并修改了标志位
    EXPECT_TRUE(notificationTriggered);
}

// 7. 测试 Batch Requests (批量打包请求)
TEST_F(JsonRpcServiceTest, ProcessesBatchRequestsCorrectly) {
    // 打包三个请求：2 个常规请求，1 个通知
    std::string request = R"([
        {"jsonrpc": "2.0", "method": "add", "params": [1, 2], "id": 10},
        {"jsonrpc": "2.0", "method": "notify_test"},
        {"jsonrpc": "2.0", "method": "add", "params": [10, 20], "id": 20}
    ])";

    EXPECT_FALSE(notificationTriggered);
    std::string responseStr = service.dispatch(request);
    
    ASSERT_FALSE(responseStr.empty());
    nlohmann::json response = nlohmann::json::parse(responseStr);
    
    // 断言响应是一个包含 2 个结果的数组（Notification 没有响应，不包含在内）
    ASSERT_TRUE(response.is_array());
    ASSERT_EQ(response.size(), 2);
    
    EXPECT_EQ(response[0]["id"], 10);
    EXPECT_EQ(response[0]["result"], 3);
    
    EXPECT_EQ(response[1]["id"], 20);
    EXPECT_EQ(response[1]["result"], 30);
    
    // 验证通知逻辑也在此批量中被正确执行
    EXPECT_TRUE(notificationTriggered);
}
