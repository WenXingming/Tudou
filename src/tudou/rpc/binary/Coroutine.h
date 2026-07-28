// ============================================================================
// Coroutine 封装二进制 RPC 客户端使用的 Boost 有栈协程。
// 它只保存执行上下文及所属 EventLoop，不负责网络或请求状态。
// ============================================================================

#pragma once

#include <functional>
#include <memory>

#include <boost/coroutine2/all.hpp>

class EventLoop;

namespace tudou {
namespace rpc {
namespace binary {

class Coroutine : public std::enable_shared_from_this<Coroutine> {
public:
    using Context = boost::coroutines2::coroutine<void>;

    Coroutine(EventLoop* loop, std::function<void()> function);
    ~Coroutine();

    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    void resume();
    void yield();

    EventLoop* get_loop() const;
    static Coroutine* current();

private:
    static thread_local Coroutine* currentCoroutine_;

    EventLoop* loop_;
    std::function<void()> function_;
    std::unique_ptr<Context::pull_type> pull_;
    Context::push_type* push_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
