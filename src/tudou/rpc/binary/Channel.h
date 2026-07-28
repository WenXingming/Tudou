// ============================================================================
// Channel 是阻塞式 Protobuf RpcChannel，支持多线程共享单条 TCP 连接。
// 后台接收线程负责收包；调用线程通过 sequenceId 匹配响应并等待 Future。
// ============================================================================

#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <google/protobuf/service.h>

#include "base/ScopedFd.h"
#include "tudou/rpc/binary/Connection.h"
#include "tudou/rpc/binary/Multiplexer.h"

namespace tudou {
namespace rpc {
namespace binary {

class Channel : public google::protobuf::RpcChannel {
public:
    Channel(const std::string& ip, uint16_t port);
    ~Channel() override;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

private:
    void receive_loop();
    bool complete_responses(const std::vector<Frame>& frames);

    void write_request(const std::string& bytes);
    static std::string encode_request(const google::protobuf::MethodDescriptor* method,
                                      const google::protobuf::Message* request,
                                      uint64_t sequenceId);

    void fail_pending_calls(const std::string& reason);

private:
    ScopedFd socket_;
    Multiplexer multiplexer_;
    Connection responseConnection_;

    std::thread receiverThread_;
    std::mutex sendMutex_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
