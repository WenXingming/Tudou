// ============================================================================
// Multiplexer 用 sequenceId 关联并发请求与乱序响应。
// 它只管理未完成调用，不知道 Socket、Protobuf、Future 或协程。
// ============================================================================

#pragma once

#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tudou {
namespace rpc {
namespace binary {

class Multiplexer {
public:
    using Completion = std::function<void(const std::string&, std::exception_ptr)>;

    Multiplexer();
    ~Multiplexer();

    Multiplexer(const Multiplexer&) = delete;
    Multiplexer& operator=(const Multiplexer&) = delete;

    uint64_t register_call(Completion completion);
    void cancel_call(uint64_t sequenceId);
    bool complete_call(uint64_t sequenceId, const std::string& responseBody);

    void close(std::exception_ptr error);
    bool is_open() const;

private:
    std::mutex mutex_;
    std::unordered_map<uint64_t, Completion> pendingCalls_;
    uint64_t nextSequenceId_;
    std::atomic<bool> open_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
