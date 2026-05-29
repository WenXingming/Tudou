// ============================================================================
// Timer.cpp
// Timer 只保存单个定时器状态，不承担任何队列管理职责。
// ============================================================================

#include "tudou/timer/Timer.h"

#include <spdlog/spdlog.h>

Timer::Timer(TimerId id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval)
    : id_(id)
    , callback_(std::move(callback))
    , expiration_(expiration)
    , interval_(interval) {
}

void Timer::run() const {
    if (!callback_) {
        spdlog::error("Timer {} has no callback to run", id_.value());
        return;
    }
    callback_();
}

void Timer::restart(Timestamp now) {
    expiration_ = now + interval_;
}
