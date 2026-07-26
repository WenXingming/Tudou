// ============================================================================
// EventLoop 在所属线程中驱动 Poller、Channel 回调和定时任务。
// 它负责跨线程唤醒与任务投递，但不拥有业务连接。
// ============================================================================

#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "base/ScopedFd.h"
#include "tudou/timer/Timer.h"

class EpollPoller;
class Channel;
class TimerQueue;
class EventLoop {
public:
    using Functor = std::function<void()>;

    explicit EventLoop(int pollTimeoutMs = 10000);
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop(); // EventLoop 主入口：poll 并执行本轮任务。
    void quit();

    void update_channel(Channel* channel) const;
    void remove_channel(Channel* channel) const;
    bool has_channel(Channel* channel) const;

    // 在 EventLoop 所属线程中执行有线程约束的任务，若当前线程不是 EventLoop 所属线程，则将回调投递到 EventLoop 的 pending queue。
    bool is_in_loop_thread() const;
    void run_in_loop(const Functor& cb);
    void queue_in_loop(const Functor& cb);

    // 定时任务接口统一委托给 TimerQueue。
    TimerId run_at(std::chrono::steady_clock::time_point when, const Functor& cb);
    TimerId run_after(double delaySeconds, const Functor& cb);
    TimerId run_every(double intervalSeconds, const Functor& cb);
    void cancel(TimerId timerId);

private:
    using FunctorQueue = std::queue<Functor>;

    void wakeup(); // 通过 eventfd 打断阻塞中的 poll。
    void on_read(); // 消费 wakeupFd_ 事件，避免重复通知。

    void do_pending_functors(); // 执行当前批次待处理任务。

private:
    thread_local static EventLoop* loopInThisThread;    // 线程局部 EventLoop 指针，强制执行 one loop per thread 约束。必须是静态的才能在所有同线程实例间共享这个检查
    const std::thread::id threadId_;                    // EventLoop 所属线程 ID，用于线程归属断言。

    const int pollTimeoutMs_;
    std::unique_ptr<EpollPoller> poller_;               // 当前线程的 Poller 实现。

    std::atomic<bool> isLooping_;                       // 当前事件循环是否处于运行状态，用于禁止 loop() 重入。
    std::atomic<bool> isQuit_;                          // 当前事件循环是否收到退出请求。

    ScopedFd wakeupFd_;                                 // 跨线程唤醒使用的 eventfd，声明在 wakeupChannel_ 之前，保证逆序析构时 Channel 先注销再关闭 fd。
    std::unique_ptr<Channel> wakeupChannel_;            // 负责监听 wakeupFd_ 可读事件的 Channel。

    FunctorQueue pendingFunctors_;                      // 待回到 EventLoop 线程执行的任务队列。
    std::mutex pendingFunctorsMutex_;                   // 保护 pendingFunctors_ 的互斥锁。
    std::atomic<bool> isCallingPendingFunctors_;        // 当前是否正在执行一批待处理任务。

    std::unique_ptr<TimerQueue> timerQueue_;      // 负责所有定时任务的 timerfd 封装层。
};
