// ============================================================================
// 基于 Linux timerfd 的定时器队列。它把最近的到期时间注册为 EventLoop
// 的可读事件，并在 EventLoop 线程内执行定时器回调。
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

#include "base/ScopedFd.h"
#include "tudou/timer/Timer.h"

class Channel;
class EventLoop;

class TimerQueue {
public:
    using Timestamp = std::chrono::steady_clock::time_point;
    using TimerEntry = std::pair<Timestamp, std::shared_ptr<Timer>>;
    using ExpiredTimers = std::vector<std::shared_ptr<Timer>>;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();
    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    TimerId add_timer(std::function<void()> callback, Timestamp when, std::chrono::milliseconds interval);
    void erase_timer(TimerId timerId);

private:
    void on_timerfd_read();
    void read_timerfd(int timerFd);

    ExpiredTimers collect_expired_timers(Timestamp now);
    void process_expired_timers(const ExpiredTimers& expiredTimers);

    void sync_timerfd();                         // 根据最早到期时间同步 timerfd；队列为空时解除武装。
    void reset_timerfd(Timestamp expiration);    // 将 timerfd 设置为指定的下一次到期时间。
    void disarm_timerfd();                       // 解除 timerfd 武装，避免空队列持续唤醒。

    int create_timerfd();

private:
    EventLoop* loop_;                                                                       // 所属 EventLoop，所有索引操作都在该线程执行。
    ScopedFd timerFd_;                                                                      // 将最近的到期时间转换为可读事件。
    std::unique_ptr<Channel> timerChannel_;                                                 // 监听 timerfd 可读事件的 Channel。

    std::atomic<uint64_t> nextTimerId_;                                                     // 跨线程生成单调递增的定时器 ID。

    std::set<TimerEntry> expireSet_;                                                        // 按到期时间排序，用于找到最近的定时器。
    std::map<TimerId, std::shared_ptr<Timer>> timersById_;                                  // 按 ID 索引，用于取消定时器。
};
