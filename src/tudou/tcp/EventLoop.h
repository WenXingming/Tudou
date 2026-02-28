/**
 * @file EventLoop.h
 * @brief 事件循环（Reactor）核心类，驱动 I/O 事件的收集与回调执行。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * 职责：持有 EpollPoller，通过 loop() 持续监听和分发事件；暴露
 * update_channel()/remove_channel() 供 Channel 注册/注销；通过 wakeupFd + pendingFunctors 支持跨线程任务投递。
 *
 * 线程模型：One Loop Per Thread，禁止拷贝、赋值。多数方法须在所属线程调用，跨线程通过 run_in_loop()/queue_in_loop() 投递。
 */

#pragma once
#include <memory>
#include <queue>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

class EpollPoller; // 前向声明，避免循环依赖
class Channel;
class EventLoop {
public:
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void loop(int timeoutMs = pollTimeoutMs_);
    void update_channel(Channel* channel) const;
    void remove_channel(Channel* channel) const;
    bool has_channel(Channel* channel) const;

    void quit();

    bool is_in_loop_thread() const;                         // 判断是否在当前线程调用
    void assert_in_loop_thread() const;                     // 确保在当前线程调用

    void run_in_loop(const std::function<void()>& cb);      // 如果在 loop 线程，直接执行 cb，否则入队
    void queue_in_loop(const std::function<void()>& cb);    // 将函数入队到 pendingFunctors 中，唤醒 loop 线程在相应线程执行 cb

private:
    int create_wakeup_fd();                                 // 创建 eventfd，用于跨线程唤醒 loop 线程
    void wakeup();                                          // 写 eventfd 打断 poll 阻塞（没有事件时会阻塞），使 loop 线程及时处理 pendingFunctors
    void on_read();                                         // wakeupChannel 的读回调，消费 eventfd 数据
    void do_pending_functors();                             // 执行 pendingFunctors 中的函数

private:
    thread_local static EventLoop* loopInthisThread;        // 线程局部变量，指向当前线程的 EventLoop 实例（One Loop Per Thread 保证）
    static const int pollTimeoutMs_;

    std::unique_ptr<EpollPoller> poller_;
    std::atomic<bool> isLooping_;
    std::atomic<bool> isQuit_;
    const std::thread::id threadId_;                        // EventLoop 所属线程 ID，辅助 One Loop Per Thread 和线程安全检查

    int wakeupFd_;
    std::unique_ptr<Channel> wakeupChannel_;

    std::queue<std::function<void()>> pendingFunctors_;     // 存放 loop 线程需要执行的函数列表
    std::atomic<bool> isCallingPendingFunctors_;
    std::mutex mtx_;                                        // 保护 pendingFunctors 的互斥锁（多线程只要有写入操作就需要加锁）
};
