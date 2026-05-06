// ============================================================================
// EventLoopThreadPool.h
// EventLoop 线程池，负责主循环与多个 IO 子循环的创建、启动与轮询分发。
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
    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;
    ~EventLoopThreadPool();

    /**
     * @brief 启动所有 IO 线程。
     */
    void start();

    /**
     * @brief 获取主事件循环。
     * @return EventLoop* 主线程中的 EventLoop。
     */
    EventLoop* get_main_loop() { return mainLoop_.get(); }

    /**
     * @brief 轮询获取下一个 IO 事件循环；无 IO 线程时回退到主循环。
     * @return EventLoop* 本次选中的事件循环。
     */
    EventLoop* get_next_loop();

    /**
     * @brief 获取当前线程池中的所有事件循环。
     * @return std::vector<EventLoop*> 包含 main loop 与所有 IO loop 的列表。
     */
    std::vector<EventLoop*> get_all_loops() const;

    /**
     * @brief 获取线程池名称。
     * @return 当前线程池名称。
     */
    std::string get_name() const { return name_; }

    /**
     * @brief 获取线程池中的事件循环总数。
     * @return 包括 main loop 在内的总事件循环数量。
     */
    int get_num_threads() const { return numThreads_ + 1; }

private:
    /**
     * @brief 创建所有 IO 线程并启动其 EventLoop。
     */
    void create_io_threads();

    /**
     * @brief 在没有 IO 线程时执行主循环初始化回调。
     */
    void initialize_main_loop_if_needed() const;

private:
    std::unique_ptr<EventLoop> mainLoop_; // 主线程中的 EventLoop。

    std::vector<std::unique_ptr<EventLoopThread>> ioLoopThreads_; // 后台 IO 线程集合。
    size_t ioLoopsIndex_; // 轮询选择 IO loop 时使用的当前索引。

    std::string name_; // 线程池名称。
    int numThreads_; // 后台 IO 线程数量。
    ThreadInitCallback initCallback_; // IO loop 初始化回调。
    bool started_; // 是否已经完成启动。
};
