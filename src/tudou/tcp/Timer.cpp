#include "Timer.h"

Timer::Timer(uint64_t id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval)
    : id_(id)
    , callback_(std::move(callback))
    , expiration_(expiration)
    , interval_(interval) {
}

void Timer::run() const {
    callback_();
}

void Timer::restart(Timestamp now) {
    expiration_ = now + interval_;
}
