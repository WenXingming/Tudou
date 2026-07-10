/**
 * @file Coroutine.cpp
 * @brief 基于 Boost.Coroutine2 的有栈协程上下文实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "Coroutine.h"

namespace tudou {
namespace rpc {

// 初始化线程局部静态变量
thread_local Coroutine* Coroutine::t_current_coroutine = nullptr;

Coroutine::Coroutine(EventLoop* loop, std::function<void()> func)
    : loop_(loop), func_(std::move(func)) {
    
    pull_ = std::make_unique<coro_t::pull_type>(
        [this](coro_t::push_type& yield) {
            push_ = &yield;
            
            // 立即挂起以返回构造函数，确保外部可以安全构造 std::shared_ptr 并使用 shared_from_this()
            (*push_)();
            
            Coroutine* saved = t_current_coroutine;
            t_current_coroutine = this;
            
            if (func_) {
                func_();
            }
            
            t_current_coroutine = saved;
        }
    );
}

Coroutine::~Coroutine() = default;

void Coroutine::resume() {
    if (pull_ && *pull_) {
        Coroutine* saved = t_current_coroutine;
        t_current_coroutine = this;
        // 恢复 pull_type 的执行流
        (*pull_)();
        t_current_coroutine = saved;
    }
}

void Coroutine::yield() {
    if (push_) {
        Coroutine* saved = t_current_coroutine;
        t_current_coroutine = nullptr;
        // 唤起 push_type 的等待（挂起当前协程，切回 resume() 的调用者线程）
        (*push_)();
        t_current_coroutine = saved;
    }
}

} // namespace rpc
} // namespace tudou
