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
    // 提供两个相互配合的类型：
    // pull_type 从外部恢复协程，push_type 从协程内部切回调用方。
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
    static thread_local Coroutine* currentCoroutine_; // 当前线程正在执行的协程，非 owning。

    EventLoop* loop_;
    std::function<void()> function_;            // 协程真正要执行的任务函数
    std::unique_ptr<Context::pull_type> pull_;  // 拥有协程栈和外部恢复入口。
    Context::push_type* push_;                  // 协程内部的挂起端点，非 owning。
};

} // namespace binary
} // namespace rpc
} // namespace tudou
