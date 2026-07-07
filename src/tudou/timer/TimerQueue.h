// ============================================================================
// TimerQueue.h
// 基于 Linux timerfd 的定时器队列：将到期时间映射为 epoll 可读事件，在 EventLoop
// 线程内单线程执行所有定时任务。双索引（按到期时间 + 按 ID）支持 O(log n) 增删。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// TimerQueue.h
// └── TimerQueue
//     ├── TimerQueue(loop)                         # [公有] 构造：创建 timerfd + Channel，并绑定读回调
//     │   ├── create_timerfd()                     # [私有] 创建 MONOTONIC timerfd 作为定时事件源
//     │   └── on_timerfd_read()                    # [私有] 绑定为 timerfd 可读时的唯一调度入口
//     │       ├── read_timerfd(timerFd_)           # [私有] 消费 timerfd 读事件，避免 epoll 反复通知
//     │       ├── 收集所有已到期定时器到本地数组      # [私有] 先从时间索引摘出，再统一执行
//     │       ├── 执行回调并处理 repeat/erase 逻辑   # [私有] 在当前 EventLoop 线程内重插或删除
//     │       └── sync_timerfd()                   # [私有] 以新的最早到期时间重置 timerfd
//     ├── TimerQueue(copy)                         # [公有] 删除拷贝构造，避免复制 timerfd 与双索引状态
//     ├── operator=(copy)                          # [公有] 删除拷贝赋值，保持队列归属唯一
//     ├── ~TimerQueue()                            # [公有] 析构：成员按声明逆序自动回收（timerChannel → timerFd）
//     ├── add_timer(callback, when, interval)      # [公有] 统一注册入口：生成 ID 后投递到 EventLoop 线程
//     │   └── sync_timerfd()                       # [私有] 若最早到期时间变化则重武装 timerfd
//     │       ├── reset_timerfd(expiration)        # [私有] 设置下次唤醒时刻
//     │       └── disarm_timerfd()                 # [私有] 队列为空时解除武装
//     ├── erase_timer(timerId)                     # [公有] 统一删除入口：按 ID 从双索引中移除
//     │   └── sync_timerfd()                       # [私有] 删除后重新同步最早到期时间
//
// ============================================================================

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "base/ScopedFd.h"
#include "tudou/timer/Timer.h"

class Channel;
class EventLoop;

class TimerQueue {
public:
    using Timestamp = std::chrono::steady_clock::time_point;
    using TimerEntry = std::pair<Timestamp, std::shared_ptr<Timer>>;

    explicit TimerQueue(EventLoop* loop);
    ~TimerQueue();
    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    TimerId add_timer(std::function<void()> callback, Timestamp when, std::chrono::milliseconds interval);
    void erase_timer(TimerId timerId);

private:
    void on_timerfd_read(); // timerfd 可读后的统一处理入口，注册到 channel

    int create_timerfd();
    void sync_timerfd();
    void reset_timerfd(Timestamp expiration);
    void disarm_timerfd();

    void read_timerfd(int timerFd);

private:
    EventLoop* loop_;                                                                       // 所属 EventLoop，限定所有索引操作的线程边界。
    ScopedFd timerFd_;                                                                      // timerfd 负责把最早到期时间映射成可读事件，声明在 timerChannel_ 之前，保证构造顺序和析构逆序
    std::unique_ptr<Channel> timerChannel_;                                                 // 监听 timerfd 可读事件的 Channel。

    std::atomic<uint64_t> nextTimerId_;                                                     // 跨线程注册定时器时使用的单调递增 ID 生成器。

    std::set<TimerEntry> expireSet_;                                                        // 按到期时间升序排序的辅助结构
    std::map<TimerId, std::shared_ptr<Timer>> timersById_;                                  // times 的管理结构
};
