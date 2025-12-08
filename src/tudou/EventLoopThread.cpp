#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread() = default;

EventLoopThread::~EventLoopThread() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        exiting = true;
    }
    if (loop) {
        loop->set_is_looping(false);
    }
    if (thread.joinable()) {
        thread.join();
    }
}

EventLoop* EventLoopThread::start_loop() {
    thread = std::thread(&EventLoopThread::thread_func, this);

    std::unique_lock<std::mutex> lock(mutex);
    cond.wait(lock, [this]() { return loop != nullptr; });
    return loop;
}

void EventLoopThread::thread_func() {
    EventLoop eventLoop;

    {
        std::lock_guard<std::mutex> lock(mutex);
        loop = &eventLoop;
        cond.notify_one();
    }

    eventLoop.loop();

    {
        std::lock_guard<std::mutex> lock(mutex);
        loop = nullptr;
    }
}
