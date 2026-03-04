#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

// TimerId is a simple wrapper around a uint64_t value that represents a unique identifier for a timer.
// 安全地标识定时器，避免直接使用裸整数，提供更清晰的接口和类型安全
class TimerId {
public:
    TimerId() : value_(0) {}
    explicit TimerId(uint64_t value) : value_(value) {}

    bool valid() const { return value_ != 0; }
    uint64_t value() const { return value_; }

private:
    uint64_t value_;
};


// Timer 类表示一个定时器对象，包含定时器的唯一标识符、回调函数、到期时间和重复间隔等信息
// 提供了执行定时器回调和重启定时器的接口，适用于单次和重复定时器的管理
class Timer {
public:
    using Callback = std::function<void()>;
    using Timestamp = std::chrono::steady_clock::time_point; // 使用 steady_clock 避免系统时间调整带来的问题

    Timer(uint64_t id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval);

    uint64_t id() const { return id_; }
    Timestamp expiration() const { return expiration_; }
    bool is_repeat() const { return interval_.count() > 0; }

    void run() const;               // 提供执行定时器回调的接口
    void restart(Timestamp now);    // 提供重启定时器的接口，更新到期时间为当前时间加上间隔，适用于重复定时器

private:
    uint64_t id_;                           // 定时器的唯一标识符
    Callback callback_;                     // 定时器到期时要执行的回调函数
    Timestamp expiration_;                  // 定时器的到期时间点
    std::chrono::milliseconds interval_;    // 定时器的重复间隔，如果为 0 表示不重复执行
};
