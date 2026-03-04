#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>

#include "Timer.h"

class Channel;
class EventLoop;

class TimerQueue {
public:
    using Timestamp = std::chrono::steady_clock::time_point;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    TimerId add_timer(const Timer::Callback& callback, Timestamp when, std::chrono::milliseconds interval);
    void erase_timer(TimerId timerId);

private:
    void add_timer_in_loop(const std::shared_ptr<Timer>& timer);
    void erase_timer_in_loop(TimerId timerId);

    void on_timerfd_read(Channel& channel);   // 定时器触发时的回调函数
    void reset_timerfd(Timestamp expiration); // 更新底层 Linux timerfd 的 timerfd 的到期时间为下一个即将到期的定时器的到期时间

    static int create_timerfd();
    static void read_timerfd(int timerFd);

private:
    EventLoop* loop_;                       // 所属的 EventLoop，TimerQueue 依赖于 EventLoop 来执行定时器回调和管理定时器生命周期
    int timerFd_;                           // 定时器使用的 timerfd 文件描述符，通过它来实现定时器的触发和管理。内核到期后内核让 timerfd 可读，EventLoop 监听到可读事件后调用 on_timerfd_read 处理定时器事件，天然适配 epoll
    std::unique_ptr<Channel> timerChannel_; // 绑定 timerFd_ 的 Channel 对象，用于在定时器触发时调用 on_timerfd_read 处理定时器事件

    uint64_t nextTimerId_;                  // 用于生成定时器的唯一 ID，每次添加定时器时递增，确保每个定时器都有一个唯一的标识符
    std::map<std::pair<Timestamp, uint64_t>, std::shared_ptr<Timer>> timersByExpire_; // 按照到期时间和定时器 ID 进行排序的定时器集合，方便快速找到下一个即将到期的定时器
    std::map<uint64_t, std::shared_ptr<Timer>> timersById_;                           // 通过定时器 ID 快速查找定时器的集合，方便取消定时器时快速定位到对应的定时器对象
};
