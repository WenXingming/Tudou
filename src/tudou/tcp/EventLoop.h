// ============================================================================
// EventLoop.h
// Reactor 核心事件循环，负责事件循环、事件分发，异步投递的任务执行和定时任务编排。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// EventLoop.h
// └── EventLoop
//     ├── EventLoop()                              # [公有] 构造：创建 Poller、eventfd + wakeup Channel 和 TimerQueue
//     │   └── on_read()                            # [私有] 绑定为 wakeupChannel_ 读回调，负责消费唤醒事件
//     ├── ~EventLoop()                             # [公有] 析构：校验线程归属，显式销毁 TimerQueue/wakeupChannel 并关闭 wakeupFd
//     ├── EventLoop(copy)                          # [公有] 删除拷贝构造，维持 one loop per thread 约束
//     ├── operator=(copy)                          # [公有] 删除拷贝赋值，禁止复制内部 Poller/TimerQueue 状态
//     ├── loop()                                   # [公有] Reactor 主循环：poll 获取就绪 Channel 列表、分发事件、执行待处理任务
//     │   └── do_pending_functors()                # [私有] 固定走“摘队列 -> 顺序执行”路径
//     ├── run_in_loop(cb)                          # [公有] 同线程直执，异线程转为异步投递
//     │   └── queue_in_loop(cb)                    # [公有] 入队并按需唤醒所属 loop 线程
//     │       └── wakeup()                         # [私有] 跨线程投递或重入投递时打断阻塞 poll
//     ├── queue_in_loop(cb)                        # [公有] 把任务放入 pendingFunctors_ 并按需唤醒
//     │   └── wakeup()                             # [私有] 保证任务不会拖到下一轮 poll
//     ├── quit()                                   # [公有] 请求退出事件循环
//     │   └── wakeup()                             # [私有] 非所属线程调用时强制唤醒 loop
//     ├── run_at(when, cb)                         # [公有] 在指定时间点执行一次性任务，底层委托 TimerQueue
//     ├── run_after(delaySeconds, cb)              # [公有] 注册一次性定时任务，底层委托 TimerQueue
//     ├── run_every(intervalSeconds, cb)           # [公有] 注册周期定时任务，底层委托 TimerQueue
//     ├── cancel(timerId)                          # [公有] 取消指定定时器，底层委托 TimerQueue
//     ├── update_channel(channel) const            # [公有] 把 Channel 事件兴趣同步到 Poller
//     ├── remove_channel(channel) const            # [公有] 从 Poller 中注销 Channel
//     ├── has_channel(channel) const               # [公有] 查询 Poller 是否已经持有该 Channel
//     └── is_in_loop_thread() const                # [公有] 判断当前线程是否就是所属 loop 线程
// ============================================================================

#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "Timer.h"

class EpollPoller;
class Channel;

// EventLoop 负责把 poll、跨线程唤醒和定时任务执行压平成单线程可读流程。
class EventLoop {
public:
    using Functor = std::function<void()>;

    EventLoop(int pollTimeoutMs = 10000);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop(); // EventLoop 主入口：poll 并执行本轮任务。
    void update_channel(Channel* channel) const;
    void remove_channel(Channel* channel) const;
    bool has_channel(Channel* channel) const;
    void quit();
    bool is_in_loop_thread() const;
    void run_in_loop(const Functor& cb); // 同线程直执，跨线程转入 pending queue。
    void queue_in_loop(const Functor& cb); // 异步投递任务，必要时唤醒阻塞中的 poll。

    using Timestamp = std::chrono::steady_clock::time_point;

    TimerId run_at(Timestamp when, const Functor& cb); // 在指定时间点执行一次性定时任务。
    TimerId run_after(double delaySeconds, const Functor& cb); // 注册一次性定时任务。
    TimerId run_every(double intervalSeconds, const Functor& cb); // 注册周期定时任务。
    void cancel(TimerId timerId);

private:
    using FunctorQueue = std::queue<Functor>;

    void wakeup(); // 通过 eventfd 打断阻塞中的 poll。
    void on_read(); // 消费 wakeupFd_ 事件，避免重复通知。
    void do_pending_functors(); // 执行当前批次待处理任务。

private:
    thread_local static EventLoop* loopInthisThread; // 线程局部 EventLoop 指针，强制执行 one loop per thread 约束。必须是静态的才能在所有同线程实例间共享这个检查
    const std::thread::id threadId_; // EventLoop 所属线程 ID，用于线程归属断言。

    const int pollTimeoutMs_;
    std::unique_ptr<EpollPoller> poller_; // 当前线程的 Poller 实现。

    std::atomic<bool> isLooping_; // 当前事件循环是否处于运行状态。
    std::atomic<bool> isQuit_; // 当前事件循环是否收到退出请求。

    int wakeupFd_; // 跨线程唤醒使用的 eventfd。
    std::unique_ptr<Channel> wakeupChannel_; // 负责监听 wakeupFd_ 可读事件的 Channel。

    FunctorQueue pendingFunctors_; // 待回到 EventLoop 线程执行的任务队列。
    std::atomic<bool> isCallingPendingFunctors_; // 当前是否正在执行一批待处理任务。
    std::mutex pendingFunctorsMutex_; // 保护 pendingFunctors_ 的互斥锁。

    std::unique_ptr<class TimerQueue> timerQueue_; // 负责所有定时任务的 timerfd 封装层。
};
