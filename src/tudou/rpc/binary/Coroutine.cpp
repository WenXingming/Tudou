// ============================================================================
// Coroutine 构造时先挂起，外部 shared_ptr 建立后才执行用户函数。
// ============================================================================

#include "tudou/rpc/binary/Coroutine.h"

#include <utility>

namespace tudou {
namespace rpc {
namespace binary {

thread_local Coroutine* Coroutine::currentCoroutine_ = nullptr;

Coroutine::Coroutine(EventLoop* loop, std::function<void()> function)
    : loop_(loop),
      function_(std::move(function)),
      pull_(nullptr),
      push_(nullptr) {
    pull_ = std::make_unique<Context::pull_type>(
        [this](Context::push_type& yield) {
            push_ = &yield;
            (*push_)();

            Coroutine* previous = currentCoroutine_;
            currentCoroutine_ = this;
            try {
                function_();
                currentCoroutine_ = previous;
            }
            catch (...) {
                currentCoroutine_ = previous;
                throw;
            }
        });
}

Coroutine::~Coroutine() = default;

void Coroutine::resume() {
    if (!pull_ || !*pull_) {
        return;
    }

    Coroutine* previous = currentCoroutine_;
    currentCoroutine_ = this;
    try {
        (*pull_)();
        currentCoroutine_ = previous;
    }
    catch (...) {
        currentCoroutine_ = previous;
        throw;
    }
}

void Coroutine::yield() {
    if (!push_) {
        return;
    }

    Coroutine* previous = currentCoroutine_;
    currentCoroutine_ = nullptr;
    try {
        (*push_)();
        currentCoroutine_ = previous;
    }
    catch (...) {
        currentCoroutine_ = previous;
        throw;
    }
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
