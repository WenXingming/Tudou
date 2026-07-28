// ============================================================================
// 验证 binary::Router 的单请求接口、反射路由与同步 Service 契约。
// ============================================================================

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "test.pb.h"
#include "tudou/rpc/binary/Router.h"

namespace tudou {
namespace rpc {
namespace test {

namespace {

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

class IncompleteService : public TestEchoService {
public:
    void Echo(google::protobuf::RpcController*,
              const EchoRequest*,
              EchoResponse*,
              google::protobuf::Closure*) override {
    }
};

std::string serialize_request(const std::string& message) {
    EchoRequest request;
    request.set_message(message);
    return request.SerializeAsString();
}

} // namespace

TEST(BinaryRpcRouterTest, DispatchesCompleteRequest) {
    binary::Router router;
    router.register_service(std::make_shared<EchoService>());

    const binary::Request request(
        "tudou.rpc.test.TestEchoService",
        "Echo",
        serialize_request("hello"));
    const std::string responseBody = router.dispatch(request);

    EchoResponse response;
    ASSERT_TRUE(response.ParseFromString(responseBody));
    EXPECT_EQ(response.message(), "Echo: hello");
}

TEST(BinaryRpcRouterTest, RejectsUnknownService) {
    binary::Router router;
    const binary::Request request("missing", "Echo", "");
    EXPECT_THROW(router.dispatch(request), std::invalid_argument);
}

TEST(BinaryRpcRouterTest, RejectsUnknownMethod) {
    binary::Router router;
    router.register_service(std::make_shared<EchoService>());
    const binary::Request request(
        "tudou.rpc.test.TestEchoService",
        "missing",
        "");
    EXPECT_THROW(router.dispatch(request), std::invalid_argument);
}

TEST(BinaryRpcRouterTest, RejectsInvalidRequestBody) {
    binary::Router router;
    router.register_service(std::make_shared<EchoService>());
    const binary::Request request(
        "tudou.rpc.test.TestEchoService",
        "Echo",
        "\xFF\xFF\xFF\xFF");
    EXPECT_THROW(router.dispatch(request), std::invalid_argument);
}

TEST(BinaryRpcRouterTest, RejectsAsynchronousService) {
    binary::Router router;
    router.register_service(std::make_shared<IncompleteService>());
    const binary::Request request(
        "tudou.rpc.test.TestEchoService",
        "Echo",
        serialize_request("hello"));
    EXPECT_THROW(router.dispatch(request), std::runtime_error);
}

} // namespace test
} // namespace rpc
} // namespace tudou
