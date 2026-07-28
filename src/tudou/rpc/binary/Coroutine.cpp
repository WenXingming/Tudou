// ============================================================================
// Coroutine 构造时先挂起，外部 shared_ptr 建立后才执行用户函数。
// resume() 统一维护当前协程，yield() 只负责切回调用方。
// ============================================================================

#include "tudou/rpc/binary/Coroutine.h"

#include <cassert>
#include <utility>

namespace tudou {
namespace rpc {
namespace binary {

thread_local Coroutine* Coroutine::currentCoroutine_ = nullptr;

Coroutine::Coroutine(EventLoop* loop, std::function<void()> function)
    : loop_(loop), function_(std::move(function)), pull_(nullptr), push_(nullptr) {

    // 创建 pull_type 对象时，为协程分配独立栈、创建寄存器上下文，并立即执行协程函数体
    pull_ = std::make_unique<Context::pull_type>(
        [this](Context::push_type& yield) {
            // push_type 是 Boost 内部为这条协程创建的 push_type 端点对象。保存 push_type，返回调用方的入口
            push_ = &yield;
            // 构造期间主动挂起。这里立即离开协程栈，返回 pull_type 构造函数的调用方
            (*push_)();
            // 此时还没有执行。协程被 resume() 后，继续执行用户函数
            function_();
        });
}

Coroutine::~Coroutine() = default;

void Coroutine::resume() {
    assert(pull_ != nullptr);
    if (!*pull_) { // pull_type 可以转换为布尔值：true  → 协程还能继续执行， false → 协程函数已经执行完毕
        return;
    }

    // 记录当前协程，切换到本协程
    Coroutine* previous = currentCoroutine_;
    currentCoroutine_ = this;
    try {
        // 真正切换上下文，执行协程函数体。这里会保存当前调用方的执行现场，然后恢复协程的栈和寄存器
        // 第一次 resume() 会从构造期间的初始挂起点继续。后续 resume() 则会从最近一次业务 yield() 后继续。
        (*pull_)();

        // 如果协程执行到 yield()，正常执行结束；那么(*pull_)() 返回，随后执行
        currentCoroutine_ = previous;
    }
    catch (...) {
        currentCoroutine_ = previous;
        throw;
    }
}

void Coroutine::yield() {
    assert(push_ != nullptr);
    // 保存协程当前栈和寄存器 → 恢复调用 resume() 的现场 → pull_() 返回 → resume() 恢复 previous
    (*push_)();
}

EventLoop* Coroutine::get_loop() const {
    return loop_;
}

Coroutine* Coroutine::current() {
    return currentCoroutine_;
}

} // namespace binary
} // namespace rpc
} // namespace tudou
