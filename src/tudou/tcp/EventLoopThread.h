// ============================================================================
// EventLoopThread.h
// EventLoop 与工作线程绑定器，负责在线程内创建、发布并驱动单个 EventLoop。
// ============================================================================

#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class EventLoop;

// EventLoopThread 负责把一个 EventLoop 严格绑定到一个后台线程上。
class EventLoopThread {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback());
    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;
    ~EventLoopThread();

    /**
     * @brief 启动后台线程并等待 EventLoop 准备完成。
     */
    void start();

    /**
     * @brief 获取后台线程中的 EventLoop。
     * @return EventLoop* 后台线程中的 EventLoop；未启动时可能为空。
     */
    EventLoop* get_loop() const { return loop_.get(); }

private:
    /**
     * @brief 创建后台线程对象。
     */
    void launch_thread();

    /**
     * @brief 阻塞等待后台线程中的 EventLoop 完成发布。
     */
    void wait_until_loop_ready();

    /**
     * @brief 后台线程的主函数，是线程侧启动流程的唯一编排入口。
     */
    void thread_func();

    /**
     * @brief 创建线程私有的 EventLoop。
     * @return 后台线程持有的 EventLoop 对象。
     */
    std::unique_ptr<EventLoop> create_loop() const;

    /**
     * @brief 执行线程初始化回调。
     * @param loop 后台线程中的 EventLoop。
     */
    void initialize_loop(EventLoop* loop) const;

    /**
     * @brief 把后台线程中的 EventLoop 发布给外部调用方。
     * @param loop 后台线程中刚创建好的 EventLoop。
     */
    void publish_loop(std::unique_ptr<EventLoop> loop);

    /**
     * @brief 清空已经退出的 EventLoop 指针。
     */
    void clear_loop();

private:
    std::unique_ptr<EventLoop> loop_; // 后台线程内创建并发布的 EventLoop。
    std::unique_ptr<std::thread> thread_; // 执行 EventLoop 的后台线程。

    std::mutex mtx_; // 保护 loop_ 发布与清理的互斥锁。
    std::condition_variable condition_; // 等待 loop_ 准备完成的条件变量。
    ThreadInitCallback initCallback_; // EventLoop 创建后、进入 loop 前执行的初始化回调。
    bool started_; // 是否已经启动过后台线程。
};
