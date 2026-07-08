// ============================================================================
// EventLoopThreadPool.h
// EventLoop 线程池，负责主循环与多个 IO 子循环的创建、启动与轮询分发。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// EventLoopThreadPool.h
// └── EventLoopThreadPool
//     ├── EventLoopThreadPool(name, numThreads, cb)  # [公有] 构造：仅记录配置，不创建任何资源
//     ├── EventLoopThreadPool(copy)              # [公有] 删除拷贝构造，避免线程池资源被复制
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值，保持 loop 集合唯一归属
//     ├── ~EventLoopThreadPool()                 # [公有] 析构：依赖成员自动回收主 loop 与 IO 线程
//     ├── start()                                # [公有] 创建主 loop，启动全部 IO 线程
//     │   ├── create_main_loop()                 # [私有] 在当前线程创建主 EventLoop
//     │   └── create_io_threads()                # [私有] 批量创建 EventLoopThread 并 start
//     ├── get_main_loop()                        # [公有] 返回主线程 EventLoop
//     ├── get_next_loop()                        # [公有] 轮询选择下一个 IO loop，空池时回退 main loop
//     ├── get_all_loops() const                  # [公有] 收集 main loop 与全部 IO loops
//     ├── get_name() const                       # [公有] 返回线程池名称
//     └── get_num_threads() const                # [公有] 返回包含 main loop 在内的总 loop 数
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

    EventLoopThreadPool(const std::string& name = std::string(), int numThreads = 0, const ThreadInitCallback& cb = ThreadInitCallback(), bool pinCpu = false);
    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;
    ~EventLoopThreadPool();

    void start(); // 启动所有 IO 线程，并在需要时初始化 main loop。

    EventLoop* get_main_loop() { return mainLoop_.get(); }
    EventLoop* get_next_loop(); // 轮询选择一个 IO loop；空池时回退 main loop。
    std::vector<EventLoop*> get_all_loops() const;
    std::string get_name() const { return name_; }
    int get_num_threads() const { return numThreads_; }

private:
    void create_main_loop(); // 在当前线程创建主 EventLoop。
    void create_io_threads(); // 批量创建并启动后台 IO 线程。

private:
    // mainLoop_ 类型是 EventLoop 而非 EventLoopThread
    // EventLoopThread 的职责是创建一个新线程并在其上运行 EventLoop。
    // main loop 跑在调用 start() 的当前线程上，两者线程模型不同，用不同类型表达是正确的抽象边界。
    std::unique_ptr<EventLoop> mainLoop_;
    std::vector<std::unique_ptr<EventLoopThread>> ioLoopThreads_;       // 后台 IO 线程集合。
    size_t ioLoopsIndex_;                                               // 轮询选择 IO loop 时使用的当前索引。

    std::string name_;                                                  // 线程池名称。
    int numThreads_;                                                    // 后台 IO 线程数量。

    bool started_;                                                      // 是否已经完成启动。

    ThreadInitCallback initCallback_;                                   // IO loop 初始化回调。
    bool pinCpu_;                                                       // 是否启用 CPU 亲和性。
};
