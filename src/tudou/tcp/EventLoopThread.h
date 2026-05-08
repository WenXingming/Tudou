// ============================================================================
// EventLoopThread.h
// EventLoop 与工作线程绑定器，负责在线程内创建、发布并驱动单个 EventLoop。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// EventLoopThread.h
// └── EventLoopThread
//     ├── EventLoopThread(cb)                    # [公有] 保存线程初始化回调，尚不启动线程
//     ├── EventLoopThread(copy)                  # [公有] 删除拷贝构造，避免后台线程对象被复制
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值，保持线程与 loop 唯一绑定
//     ├── ~EventLoopThread()                     # [公有] 析构：请求 loop 退出并 join 后台线程
//     ├── start()                                # [公有] 启动线程并阻塞等待 EventLoop 准备完成
//     │   └── wait_for_loop()                    # [私有] 等待后台线程完成 loop_ 创建
//     └── get_loop() const                       # [公有] 返回后台线程中的 EventLoop 指针
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

    void start(); // 启动后台线程并等待 EventLoop 就绪。
    EventLoop* get_loop() const { return loop_.get(); }

private:
    void thread_func(); // 后台线程主干：创建 EventLoop、通知主线程、驱动 loop、清理。
    void signal_loop_ready(); // 通过条件变量通知主线程 EventLoop 已创建完成。
    void wait_for_loop();

private:
    std::unique_ptr<EventLoop> loop_; // 后台线程内创建并发布的 EventLoop。
    std::unique_ptr<std::thread> thread_; // 执行 EventLoop 的后台线程。

    std::mutex loopMutex_; // 保护 loop_ 并与 condition_ 配对使用（condition_.wait 要求锁的就是保护共享状态的这把锁）。
    std::condition_variable condition_; // 等待 loop_ 创建完成的条件变量。

    bool started_; // 是否已经启动过后台线程。

    ThreadInitCallback initCallback_; // EventLoop 创建后、进入 loop 前执行的初始化回调。
};
