// EventLoopThread: 一个线程配一个 EventLoop，线程函数中创建并运行 EventLoop::loop()

#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

class EventLoop;

class EventLoopThread {
public:
    EventLoopThread();
    ~EventLoopThread();

    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;

    EventLoop* start_loop();

private:
    void thread_func();

private:
    std::thread thread;
    EventLoop* loop{ nullptr };
    std::mutex mutex;
    std::condition_variable cond;
    bool exiting{ false };
};
