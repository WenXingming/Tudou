// ============================================================================
// Timer.cpp
// Timer 只保存单个定时器状态，不承担任何队列管理职责。
// ============================================================================

#include "Timer.h"

Timer::Timer(uint64_t id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval)
    : id_(id)
    , callback_(std::move(callback))
    , expiration_(expiration)
    , interval_(interval) {
}

void Timer::run() const {
    // Timer 自身只负责执行已注入回调，不判断调度时机。
    callback_();
}

void Timer::restart(Timestamp now) {
    // 重复定时器总是以“当前执行完成的时刻”为基准推进下一次到期时间。
    expiration_ = now + interval_;
}
