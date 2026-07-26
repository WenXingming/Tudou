// ============================================================================
// TimerId 用无符号整数表示定时器身份，0 表示无效 ID。
// Timer 保存单个定时器的回调、到期时间和重复周期，不参与队列调度。
// ============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

class TimerId {
public:
    TimerId() : value_(0) {}
    explicit TimerId(uint64_t value) : value_(value) {}

    bool valid() const { return value_ != 0; }
    uint64_t value() const { return value_; }

    bool operator<(const TimerId& other) const { return value_ < other.value_; }
    bool operator==(const TimerId& other) const { return value_ == other.value_; }

private:
    uint64_t value_;    // 0 表示无效
};


class Timer {
public:
    using Callback = std::function<void()>;
    using Timestamp = std::chrono::steady_clock::time_point;

    Timer(TimerId id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval);

    void run() const;
    void reschedule(Timestamp now);

    TimerId get_id() const { return id_; }
    Timestamp get_expiration() const { return expiration_; }

    bool is_repeat() const { return interval_.count() > 0; }

private:
    TimerId id_;
    Callback callback_;
    Timestamp expiration_;
    std::chrono::milliseconds interval_;    // 0 表示一次性定时器
};
