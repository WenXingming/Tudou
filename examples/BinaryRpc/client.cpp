// ============================================================================
// Binary RPC 客户端示例：协程等待响应，EventLoop 线程继续处理网络事件。
// ============================================================================

#include <iostream>
#include <memory>

#include "echo.pb.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/rpc/binary/Coroutine.h"
#include "tudou/rpc/binary/CoroutineChannel.h"

int main() {
    EventLoop loop;
    tudou::rpc::binary::CoroutineChannel channel(loop, "127.0.0.1", 8091);

    auto coroutine = std::make_shared<tudou::rpc::binary::Coroutine>(&loop, [&]() {
        // 创建 EchoService_Stub 把 Echo() 转换成远程 RPC 请求
        // Tudou 的 CoroutineChannel 继承了 Protobuf 的通用 Channel 接口
        tudou::example::EchoService_Stub stub(&channel);
        tudou::example::EchoRequest request;
        tudou::example::EchoResponse response;
        request.set_message("hello Tudou");

        // CallMethod 内部 yield；响应到达后，协程从调用点继续执行。
        // 序列化和反序列化都被 Stub 与 CoroutineChannel 隐藏了
        // stub.Echo()
        //     → CoroutineChannel::CallMethod()
        //     → request->SerializeAsString()
        //     → FrameCodec::encode()
        //     → Socket 发送
        // 具体来说，Protobuf 生成的 Stub 只负责转发
        // void EchoService_Stub::Echo(...) {
        //     channel_->CallMethod(
        //         descriptor()->method(0),
        //         controller,
        //         request,
        //         response,
        //         done);
        // }
        stub.Echo(nullptr, &request, &response, nullptr);
        std::cout << response.message() << '\n';
        loop.quit();
        });

    coroutine->resume();
    loop.loop();
}
