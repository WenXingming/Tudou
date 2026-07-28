// ============================================================================
// Binary RPC 服务端示例：实现 Protobuf Service，注册后进入 Reactor 事件循环。
// ============================================================================

#include <iostream>
#include <memory>

#include "echo.pb.h"
#include "tudou/rpc/binary/Server.h"

namespace {

// 服务端需要继承生成的 EchoService 并实现真正的 Echo() 业务逻辑
class EchoServiceImpl final : public tudou::example::EchoService {
public:
    void Echo(google::protobuf::RpcController*,
        const tudou::example::EchoRequest* request,
        tudou::example::EchoResponse* response,
        google::protobuf::Closure* done) override {
        response->set_message("Echo: " + request->message());
        done->Run();
    }
};

} // namespace

int main() {
    tudou::rpc::binary::Server server("127.0.0.1", 8091, 2);
    server.register_service(std::make_shared<EchoServiceImpl>());

    std::cout << "Binary RPC server listening on 127.0.0.1:8091\n";
    server.start();
}
