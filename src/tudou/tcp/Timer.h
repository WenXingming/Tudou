// ============================================================================
// Timer.h
// 定时器值对象与 TimerId 契约，负责描述到期时间、周期和回调本身。
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

    /**
     * @brief 判断当前标识是否指向一个真实定时器。
     * @return true 表示该标识有效。
     */
    bool valid() const { return value_ != 0; }

    /**
     * @brief 读取底层整数标识。
     * @return uint64_t 当前定时器标识值。
     */
    uint64_t value() const { return value_; }

private:
    uint64_t value_; // 底层定时器标识值，0 表示无效。
};

// Timer 负责保存单个定时器的执行契约，不参与队列调度与编排。
class Timer {
public:
    using Callback = std::function<void()>;
    using Timestamp = std::chrono::steady_clock::time_point;

    Timer(uint64_t id, Callback callback, Timestamp expiration, std::chrono::milliseconds interval);

    /**
     * @brief 获取当前定时器的唯一标识。
     * @return uint64_t 定时器 ID。
     */
    uint64_t id() const { return id_; }

    /**
     * @brief 获取当前定时器的下次到期时间。
     * @return Timestamp 下次到期时间点。
     */
    Timestamp expiration() const { return expiration_; }

    /**
     * @brief 判断当前定时器是否为重复定时器。
     * @return true 表示 interval 大于 0。
     */
    bool is_repeat() const { return interval_.count() > 0; }

    /**
     * @brief 执行定时器回调。
     */
    void run() const;

    /**
     * @brief 以当前时间为基准重启重复定时器。
     * @param now 当前时间点。
     */
    void restart(Timestamp now);

private:
    uint64_t id_; // 定时器唯一标识。
    Callback callback_; // 到期后执行的回调。
    Timestamp expiration_; // 当前记录的下次到期时间。
    std::chrono::milliseconds interval_; // 重复定时器的周期；0 表示只执行一次。
};
