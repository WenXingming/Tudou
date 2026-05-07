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
//     │   ├── launch_thread()                    # [私有] 创建后台 std::thread
//     │   │   └── thread_func()                  # [私有] 后台线程主干：创建、发布、运行并清理 EventLoop
//     │   │       ├── create_loop() const        # [私有] 在线程内构建 EventLoop 实例
//     │   │       ├── initialize_loop(loop) const  # [私有] 执行 ThreadInitCallback
//     │   │       ├── publish_loop(loop)         # [私有] 把 loop_ 发布给外部并唤醒等待方
//     │   │       └── clear_loop()               # [私有] loop 退出后回收已发布指针
//     │   └── wait_until_loop_ready()            # [私有] 等待 loop_ 发布成功
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
    void launch_thread();
    void wait_until_loop_ready();
    void thread_func(); // 创建、发布并驱动该线程专属的 EventLoop。
    std::unique_ptr<EventLoop> create_loop() const;
    void initialize_loop(EventLoop* loop) const;
    void publish_loop(std::unique_ptr<EventLoop> loop); // 对外发布已经准备好的 EventLoop。
    void clear_loop();

private:
    std::unique_ptr<EventLoop> loop_; // 后台线程内创建并发布的 EventLoop。
    std::unique_ptr<std::thread> thread_; // 执行 EventLoop 的后台线程。

    std::mutex mtx_; // 保护 loop_ 发布与清理的互斥锁。
    std::condition_variable condition_; // 等待 loop_ 准备完成的条件变量。
    ThreadInitCallback initCallback_; // EventLoop 创建后、进入 loop 前执行的初始化回调。
    bool started_; // 是否已经启动过后台线程。
};
