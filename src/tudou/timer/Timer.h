// ============================================================================
// Timer.h
// 定时器值对象与 TimerId 契约，负责描述到期时间、周期和回调本身。
//
// 成员函数调用树（[公有] 标注对外接口）：
//
// Timer.h
// ├── TimerId
// │   ├── TimerId()                             # [公有] 构造无效定时器 ID（0 表示 invalid）
// │   ├── TimerId(value)                        # [公有] 用显式数值包装一个定时器标识
// │   ├── valid() const                         # [公有] 判断当前 TimerId 是否有效
// │   ├── value() const                         # [公有] 读取裸整数 ID
// │   └── operator<(other) const                # [公有] 允许 TimerId 作为有序容器 key
// └── Timer
//     ├── Timer(id, callback, expiration, interval)  # [公有] 保存回调契约、到期时间与重复周期
//     ├── get_id() const                        # [公有] 返回定时器 ID
//     ├── get_expiration() const                # [公有] 返回当前到期时间
//     ├── is_repeat() const                     # [公有] 判断是否为周期定时器
//     ├── run() const                           # [公有] 执行已注入回调
//     └── reschedule(now)                       # [公有] 重复定时器按当前时刻推进下一次到期时间
// ============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>

// TimerId 负责把裸整数封装成明确的定时器标识契约。
class TimerId {
public:
    TimerId() : value_(0) {}
    explicit TimerId(uint64_t value) : value_(value) {}

    bool valid() const { return value_ != 0; }
    uint64_t value() const { return value_; }

    bool operator<(const TimerId& other) const { return value_ < other.value_; }
    bool operator==(const TimerId& other) const { return value_ == other.value_; }

private:
    uint64_t value_;                        // 0 表示无效。
};


// Timer 负责保存单个定时器的执行契约，不参与队列调度与编排。
class Timer {
public:
    using Callback = std::function<void()>;
    using Timestamp = std::chrono::steady_clock::time_point;

    Timer(TimerId id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval);

    TimerId get_id() const { return id_; }
    Timestamp get_expiration() const { return expiration_; }
    bool is_repeat() const { return interval_.count() > 0; }
    void run() const;
    void reschedule(Timestamp now);

private:
    TimerId id_;
    Callback callback_;
    Timestamp expiration_;
    std::chrono::milliseconds interval_;    // 0 表示一次性定时器。
};
