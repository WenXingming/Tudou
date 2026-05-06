// ============================================================================
// EventLoop.h
// Reactor 核心事件循环，负责 poll、唤醒、任务投递和定时任务编排。
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

    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    /**
     * @brief 启动事件循环并持续处理 I/O、唤醒事件和待执行任务。
     * @param timeoutMs 单次 poll 的超时时间，单位毫秒。
     */
    void loop(int timeoutMs = pollTimeoutMs_);

    /**
     * @brief 将 Channel 的关注事件同步到 Poller。
     * @param channel 需要注册或更新的 Channel。
     */
    void update_channel(Channel* channel) const;

    /**
     * @brief 将 Channel 从 Poller 中移除。
     * @param channel 需要移除的 Channel。
     */
    void remove_channel(Channel* channel) const;

    /**
     * @brief 判断当前 Poller 是否已经持有该 Channel。
     * @param channel 需要检查的 Channel。
     * @return true 表示当前 EventLoop 已注册该 Channel。
     */
    bool has_channel(Channel* channel) const;

    /**
     * @brief 请求退出事件循环。
     */
    void quit();

    /**
     * @brief 判断调用方是否位于当前 EventLoop 线程。
     * @return true 表示当前线程就是所属线程。
     */
    bool is_in_loop_thread() const;

    /**
     * @brief 断言调用方位于当前 EventLoop 线程。
     */
    void assert_in_loop_thread() const;

    /**
     * @brief 在当前 EventLoop 线程执行任务；若来自其他线程则转为异步投递。
     * @param cb 待执行任务。
     */
    void run_in_loop(const Functor& cb);

    /**
     * @brief 将任务投递到 EventLoop 所属线程异步执行。
     * @param cb 待执行任务。
     */
    void queue_in_loop(const Functor& cb);

    /**
     * @brief 在指定延迟后执行一次任务。
     * @param delaySeconds 延迟秒数。
     * @param cb 到期后执行的任务。
     * @return TimerId 新创建的定时器标识。
     */
    TimerId run_after(double delaySeconds, const Functor& cb);

    /**
     * @brief 按固定周期重复执行任务。
     * @param intervalSeconds 周期间隔，单位秒。
     * @param cb 每次到期后执行的任务。
     * @return TimerId 新创建的重复定时器标识。
     */
    TimerId run_every(double intervalSeconds, const Functor& cb);

    /**
     * @brief 取消一个定时器。
     * @param timerId 需要取消的定时器标识。
     */
    void cancel(TimerId timerId);

private:
    using FunctorQueue = std::queue<Functor>;

    /**
     * @brief 创建用于跨线程唤醒的 eventfd。
     * @return eventfd 文件描述符。
     */
    int create_wakeup_fd();

    /**
     * @brief 通过 eventfd 唤醒阻塞中的 poll。
     */
    void wakeup();

    /**
     * @brief 消费 eventfd 上的唤醒信号。
     */
    void on_read();

    /**
     * @brief 执行当前批次的待处理任务。
     */
    void do_pending_functors();

    /**
     * @brief 将当前待处理任务队列交换到局部变量中。
     * @return 一批待执行任务。
     */
    FunctorQueue take_pending_functors();

    /**
     * @brief 顺序执行一批待处理任务。
     * @param functors 本轮需要执行的任务队列。
     */
    void execute_pending_functors(FunctorQueue& functors);

private:
    thread_local static EventLoop* loopInthisThread; // 线程局部 EventLoop 指针，强制执行 one loop per thread 约束。
    static const int pollTimeoutMs_;

    std::unique_ptr<EpollPoller> poller_; // 当前线程的 Poller 实现。
    std::atomic<bool> isLooping_; // 当前事件循环是否处于运行状态。
    std::atomic<bool> isQuit_; // 当前事件循环是否收到退出请求。
    const std::thread::id threadId_; // EventLoop 所属线程 ID，用于线程归属断言。

    int wakeupFd_; // 跨线程唤醒使用的 eventfd。
    std::unique_ptr<Channel> wakeupChannel_; // 负责监听 wakeupFd_ 可读事件的 Channel。

    FunctorQueue pendingFunctors_; // 待回到 EventLoop 线程执行的任务队列。
    std::atomic<bool> isCallingPendingFunctors_; // 当前是否正在执行一批待处理任务。
    std::mutex mtx_; // 保护 pendingFunctors_ 的互斥锁。

    std::unique_ptr<class TimerQueue> timerQueue_; // 负责所有定时任务的 timerfd 封装层。
};
