// ============================================================================
// TimerQueue.h
// TimerQueue 负责把多个 Timer 编排到 timerfd 上，并以 EventLoop 线程为唯一执行入口。
// ============================================================================

#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "Timer.h"

class Channel;
class EventLoop;

// TimerQueue 负责维护 timerfd 与定时器索引，保证定时任务在 EventLoop 线程内线性执行。
class TimerQueue {
public:
    using Timestamp = std::chrono::steady_clock::time_point;
    using TimerKey = std::pair<Timestamp, uint64_t>;
    using TimerMap = std::map<TimerKey, std::shared_ptr<Timer>>;
    using TimerList = std::vector<std::shared_ptr<Timer>>;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    /**
     * @brief 新增一个定时器。
     * @param callback 到期后执行的任务。
     * @param when 首次到期时间。
     * @param interval 重复定时器的周期；0 表示一次性定时器。
     * @return TimerId 新创建的定时器标识。
     */
    TimerId add_timer(const Timer::Callback& callback, Timestamp when, std::chrono::milliseconds interval);

    /**
     * @brief 按标识移除一个定时器。
     * @param timerId 需要移除的定时器标识。
     */
    void erase_timer(TimerId timerId);

private:
    /**
     * @brief 在 EventLoop 线程内注册一个定时器。
     * @param timer 待注册的定时器对象。
     */
    void add_timer_in_loop(const std::shared_ptr<Timer>& timer);

    /**
     * @brief 在 EventLoop 线程内移除一个定时器。
     * @param timerId 需要移除的定时器标识。
     */
    void erase_timer_in_loop(TimerId timerId);

    /**
     * @brief 处理 timerfd 可读事件，是定时器执行的唯一编排入口。
     * @param channel 触发事件的 timerfd Channel。
     */
    void on_timerfd_read(Channel& channel);

    /**
     * @brief 收集所有已经到期的定时器。
     * @param now 当前时间点。
     * @return TimerList 当前批次到期的定时器集合。
     */
    TimerList collect_expired_timers(Timestamp now);

    /**
     * @brief 执行本批次到期的定时器，并按需重启重复定时器。
     * @param expiredTimers 本批次到期的定时器集合。
     * @param now 当前时间点。
     */
    void execute_expired_timers(const TimerList& expiredTimers, Timestamp now);

    /**
     * @brief 将一个重复定时器重新加入索引。
     * @param timer 需要重启的定时器对象。
     */
    void reschedule_timer(const std::shared_ptr<Timer>& timer);

    /**
     * @brief 根据当前最早到期定时器同步 timerfd。
     */
    void sync_timerfd();

    /**
     * @brief 将 timerfd 重置到指定到期时间。
     * @param expiration 下一个需要唤醒 EventLoop 的时间点。
     */
    void reset_timerfd(Timestamp expiration);

    /**
     * @brief 取消 timerfd 当前的到期设置。
     */
    void disarm_timerfd();

    /**
     * @brief 创建底层 timerfd。
     * @return timerfd 文件描述符。
     */
    static int create_timerfd();

    /**
     * @brief 读取 timerfd 计数，消费本次可读事件。
     * @param timerFd 需要读取的 timerfd。
     */
    static void read_timerfd(int timerFd);

private:
    EventLoop* loop_; // 所属 EventLoop，限定所有调度操作的线程边界。
    int timerFd_; // Linux timerfd，用来把下次到期时间映射成 epoll 可读事件。
    std::unique_ptr<Channel> timerChannel_; // 监听 timerFd_ 的 Channel。

    uint64_t nextTimerId_; // 递增生成的定时器 ID。
    TimerMap timersByExpire_; // 以到期时间排序的主索引，用于快速找到下一次唤醒时间。
    std::map<uint64_t, std::shared_ptr<Timer>> timersById_; // 以 TimerId 索引的辅助表，用于 O(log n) 取消定时器。
};
