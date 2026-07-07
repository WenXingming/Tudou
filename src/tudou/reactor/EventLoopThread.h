// ============================================================================
// EventLoopThread.h
// EventLoop 与工作线程绑定器，负责在线程内创建、发布并驱动单个 EventLoop。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// EventLoopThread.h
// └── EventLoopThread
//     ├── EventLoopThread(cb)                    # [公有] 启动后台线程，创建 EventLoop，阻塞等待就绪
//     ├── EventLoopThread(copy)                  # [公有] 删除拷贝构造，避免后台线程对象被复制
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值，保持线程与 loop 唯一绑定
//     ├── ~EventLoopThread()                     # [公有] 析构：请求 loop 退出并 join 后台线程
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

    explicit EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback());
    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;
    ~EventLoopThread();

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
