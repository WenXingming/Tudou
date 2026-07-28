// ============================================================================
// CoroutineChannel 是 EventLoop 驱动的非阻塞 Protobuf RpcChannel。
// 每次调用挂起当前 Coroutine，响应到达后在原 EventLoop 恢复。
// ============================================================================

#pragma once

#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/service.h>

#include "base/ScopedFd.h"
#include "tudou/rpc/binary/Connection.h"
#include "tudou/rpc/binary/Multiplexer.h"
#include "tudou/tcp/Buffer.h"

class Channel;
class EventLoop;

namespace tudou {
namespace rpc {
namespace binary {

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
    struct CallResult {
        CallResult() : error(nullptr) {}

        std::exception_ptr error;
    };

    void on_read();
    void on_write();
    bool complete_responses(const std::vector<Frame>& frames);

    void queue_request(const std::string& bytes);
    static std::string encode_request(const google::protobuf::MethodDescriptor* method,
                                      const google::protobuf::Message* request,
                                      uint64_t sequenceId);

    void fail_pending_calls(const std::string& reason);

private:
    ScopedFd socket_;
    EventLoop& loop_;
    std::unique_ptr<::Channel> channel_;

    Multiplexer multiplexer_;
    Connection responseConnection_;
    Buffer outputBuffer_;
    bool connected_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
