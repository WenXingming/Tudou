/**
 * @file EventLoopThreadPool.h
 * @brief 将多个 EventLoopThread 组合成线程池的封装类
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * - 之前写的通用线程池好像并不好用（我感觉应该是能用的，但是项目其他地方的线程间同步应该没做好，导致报错）...
 * - 因为 EventLoop 需要绑定线程，所以这里直接使用 EventLoopThread 来组成线程池
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>

class EventLoop;
class EventLoopThread;
class EventLoopThreadPool {
    using ThreadInitCallback = std::function<void(EventLoop*)>;

private:
    std::unique_ptr<EventLoop> mainLoop; // 监听线程的 EventLoop（之前是放在 TcpServer 里的，这里放到线程池里可能更合适一些）

    std::vector<std::unique_ptr<EventLoopThread>> ioLoopThreads; // IO 线程池
    size_t ioLoopsIndex;                                         // 轮询索引

    std::string name;                                            // 线程池名称
    int numThreads;                                              // 线程池中的 IO 线程数量（构造时确定）

    ThreadInitCallback initCallback;                             // IO 线程初始化回调
    bool started;                                                // 是否已启动

public:
    EventLoopThreadPool(const std::string& name = std::string(), int numThreads = 0, const ThreadInitCallback& cb = ThreadInitCallback());
    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;
    ~EventLoopThreadPool() = default;

    void start();

    EventLoop* get_main_loop() { return mainLoop.get(); }
    EventLoop* get_next_loop();
    std::vector<EventLoop*> get_all_loops() const;

    std::string get_name() const { return name; }
    int get_num_threads() const { return numThreads + 1; } // 包括 mainLoop
};
