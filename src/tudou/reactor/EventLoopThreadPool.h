// ============================================================================
// EventLoopThreadPool 管理一个 main loop 和多个 IO loop。
// 它负责启动线程，并从 main loop 中轮询选择 IO loop。
// ============================================================================

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

class EventLoop;
class EventLoopThread;

// EventLoopThreadPool 负责持有 main loop 和多个 IO loop，并为新连接选择目标 loop。
class EventLoopThreadPool {
public:
    using ThreadInitCallback = std::function<void(EventLoop*)>;

    EventLoopThreadPool(const std::string& name = std::string(), int numThreads = 0, const ThreadInitCallback& cb = ThreadInitCallback());
    ~EventLoopThreadPool();

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void start(); // 创建 main loop 并启动所有 IO 线程。

    EventLoop* get_next_loop(); // 轮询选择一个 IO loop；空池时回退 main loop。

    EventLoop* get_main_loop() { return mainLoop_.get(); }
    std::vector<EventLoop*> get_all_loops() const;
    std::string get_name() const { return name_; }
    int get_num_threads() const { return numThreads_ + 1; } // 返回 main loop 与 IO loop 的总数。

private:
    void create_main_loop(); // 在当前线程创建主 EventLoop。
    void create_io_threads(); // 批量创建并启动后台 IO 线程。

private:
    std::unique_ptr<EventLoop> mainLoop_;                               // 主线程内创建并发布的 EventLoop。使用指针可以延迟创建，避免在构造时自动创建
    std::vector<std::unique_ptr<EventLoopThread>> ioLoopThreads_;       // 后台 IO 线程集合。
    size_t ioLoopsIndex_;                                               // 轮询选择 IO loop 时使用的当前索引。EventLoopThread 包含锁等不可复制，所以这里使用 unique_ptr 存储。

    std::string name_;                                                  // 线程池名称。
    int numThreads_;                                                    // 后台 IO 线程数量。

    bool started_;                                                      // 是否已经完成启动。

    ThreadInitCallback initCallback_;                                   // IO loop 初始化回调。
};
