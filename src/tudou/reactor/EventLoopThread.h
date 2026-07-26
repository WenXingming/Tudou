// ============================================================================
// EventLoopThread 在后台线程中创建并驱动一个 EventLoop。
// 它负责等待 loop 初始化完成，并在析构时请求退出后 join 线程。
// ============================================================================

#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class EventLoop;

class EventLoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    explicit EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback());
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    EventLoop* get_loop();

private:
    void thread_func(); // 后台线程主干：创建 EventLoop、驱动 loop、清理。

private:
    std::unique_ptr<EventLoop> loop_;           // 后台线程内创建并发布的 EventLoop。
    std::thread thread_;                        // 执行 EventLoop 的后台线程。

    std::mutex loopMutex_;                      // 保护 loop_ 并与 loopCondition_ 配对使用。
    std::condition_variable loopCondition_;     // 等待 loop_ 创建完成的条件变量。

    ThreadInitCallback initCallback_;           // EventLoop 创建后、进入 loop 前执行的初始化回调。
};
