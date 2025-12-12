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
    using ThreadInitCallback = std::function<void(EventLoop*)>; // 线程创建后可以调用该回调函数进行一些初始化操作，如果未传入，则不进行任何操作

private:
    std::unique_ptr<EventLoop> mainLoop; // 监听线程的 EventLoop（之前是放在 TcpServer 里的，这里放到线程池里可能更合适一些）

    std::vector<std::unique_ptr<EventLoopThread>> ioLoopThreads; // IO 线程池
    // std::vector<EventLoop*> ioLoops;                          // 在一起感觉不用，EventLoopThread 提供的 get_loop() 方法也能获取到。都封装在一起了，没必要额外存一份指针数组。而且也不安全，可能会出现悬空指针的问题，EventLoopThread 里面的 EventLoop 指针比较安全一些
    size_t ioLoopsIndex;                                         // 轮询索引

    std::string name;                                            // 线程池名称
    bool started;                                                // 线程池是否已经启动
    int numThreads;                                              // 线程池中的线程数量，感觉没必要单独存这个变量了，直接用 ioLoopThreads.size() 就行

public:
    EventLoopThreadPool(const std::string& nameArg = std::string(), int numThreadsArg = 0);
    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;
    ~EventLoopThreadPool() = default;

    void start(const ThreadInitCallback& cb = ThreadInitCallback());

    EventLoop* get_main_loop() { return mainLoop.get(); }
    EventLoop* get_next_loop(); // 默认轮询算法获取下一个 IO 线程的 EventLoop
    std::vector<EventLoop*> get_all_loops() const; // 获取所有 IO 线程的 EventLoop

    std::string get_name() const { return name; }
    bool is_started() const { return started; }
    void set_thread_num(int numThreads) { this->numThreads = numThreads; }
};



// #include "EventLoop.h"
// #include "threadpool/ThreadPool.h"

// class EventLoop;
// class EventLoopThread;
// class EventLoopThreadPool {
// public:
//     explicit EventLoopThreadPool(size_t numThreads);
//     ~EventLoopThreadPool() = default;
//     EventLoopThreadPool(const EventLoopThreadPool&) = delete;
//     EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

//     void start();
//     // EventLoop* get_main_loop() { return mainLoop.get(); }
//     EventLoop* get_next_loop();

// private:
//     // std::unique_ptr<EventLoop> mainLoop;    // 主线程的 EventLoop，监听线程的 EventLoop

//     size_t numThreads;
//     wxm::ThreadPool ioLoopThreadPool;       // IO loop 属于所属线程

//     std::mutex mtx;                         // 保护共享资源 ioLoops
//     std::vector<EventLoop*> ioLoops;        // IO loops 的引用
//     size_t ioLoopsIndex;
// };
