#pragma once

#include <vector>
#include <memory>

class EventLoop;
class EventLoopThread;

class EventLoopThreadPool {
public:
    explicit EventLoopThreadPool(size_t numThreads);
    ~EventLoopThreadPool() = default;

    EventLoopThreadPool(const EventLoopThreadPool&) = delete;
    EventLoopThreadPool& operator=(const EventLoopThreadPool&) = delete;

    void start();
    EventLoop* get_next_loop();

private:
    size_t numThreads;
    std::vector<std::unique_ptr<EventLoopThread>> threads;
    std::vector<EventLoop*> loops;
    size_t next{ 0 };
};
