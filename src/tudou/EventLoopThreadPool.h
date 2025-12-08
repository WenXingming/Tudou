#pragma once

#include <vector>
#include <memory>
#include <mutex>

#include "EventLoop.h"
#include "threadpool/ThreadPool.h"

class EventLoop;
class EventLoopThread;
class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(size_t numThreads);
    ~EventLoopThreadPool() = default;
    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void start();
    // EventLoop* get_main_loop() { return mainLoop.get(); }
    EventLoop* get_next_loop();

private:
    // std::unique_ptr<EventLoop> mainLoop;    // 主线程的 EventLoop，监听线程的 EventLoop

    size_t numThreads;
    wxm::ThreadPool ioLoopThreadPool;       // IO loop 属于所属线程

    std::mutex mtx;                         // 保护共享资源 ioLoops
    std::vector<EventLoop*> ioLoops;        // IO loops 的引用
    size_t ioLoopsIndex;
};
