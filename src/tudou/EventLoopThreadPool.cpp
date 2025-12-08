#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

EventLoopThreadPool::EventLoopThreadPool(size_t numThreads)
    : numThreads(numThreads) {
}

void EventLoopThreadPool::start() {
    threads.reserve(numThreads);
    loops.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        auto t = std::make_unique<EventLoopThread>();
        EventLoop* loop = t->start_loop();
        threads.push_back(std::move(t));
        loops.push_back(loop);
    }
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    if (loops.empty()) return nullptr;
    EventLoop* loop = loops[next];
    next = (next + 1) % loops.size();
    return loop;
}
