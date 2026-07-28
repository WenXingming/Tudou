// ============================================================================
// CoroutineChannel 是 EventLoop 驱动的非阻塞 Protobuf RpcChannel。
// 每次调用挂起当前 Coroutine，响应到达后在原 EventLoop 恢复。
// ============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <google/protobuf/service.h>

#include "base/ScopedFd.h"
#include "tudou/reactor/Channel.h"
#include "tudou/rpc/binary/Connection.h"
#include "tudou/tcp/Buffer.h"

class EventLoop;

namespace tudou {
namespace rpc {
namespace binary {

class Coroutine;

class CoroutineChannel : public google::protobuf::RpcChannel {
public:
    CoroutineChannel(EventLoop& loop, const std::string& ip, uint16_t port);
    ~CoroutineChannel() override;

    CoroutineChannel(const CoroutineChannel&) = delete;
    CoroutineChannel& operator=(const CoroutineChannel&) = delete;

    void CallMethod(const google::protobuf::MethodDescriptor* method,
        google::protobuf::RpcController* controller,
        const google::protobuf::Message* request,
        google::protobuf::Message* response,
        google::protobuf::Closure* done) override;

private:
    struct PendingCall {
        google::protobuf::Message* response;       // 指向挂起协程栈上的响应对象。
        std::shared_ptr<Coroutine> coroutine;       // 保证 response 和 error 所在的协程栈存活。
        std::string* error;                         // 指向挂起协程栈上的错误结果。
    };

    void on_read();
    bool complete_responses(const std::vector<Frame>& frames);

    void on_write();

    static std::string encode_request(const google::protobuf::MethodDescriptor* method,
        const google::protobuf::Message* request,
        uint64_t sequenceId);

    void fail_pending_calls(const std::string& reason);

private:
    EventLoop& loop_;
    ScopedFd socket_;
    Channel channel_;

    Connection responseConnection_;
    Buffer outputBuffer_;
    std::unordered_map<uint64_t, PendingCall> pendingCalls_;
    uint64_t nextSequenceId_;

    bool connected_;
    bool open_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
