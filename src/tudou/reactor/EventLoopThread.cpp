// ============================================================================
// EventLoopThread.cpp
// EventLoop 线程绑定器实现。
// ============================================================================

#include "tudou/reactor/EventLoopThread.h"

#include "tudou/reactor/EventLoop.h"

#include <cassert>

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb)
    : loop_(nullptr)
    , thread_(nullptr)
    , loopMutex_()
    , condition_()
    , initCallback_(cb) {

}

EventLoopThread::~EventLoopThread() {
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        if (loop_) {
            // 先请求 loop 退出，再由 join 等待线程把资源完整收口。
            loop_->quit();
        }
    }
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void EventLoopThread::start() {
    thread_ = std::make_unique<std::thread>(&EventLoopThread::thread_func, this);

    wait_for_loop();
}

void EventLoopThread::thread_func() {
    // 创建该线程专属的 EventLoop，并通过条件变量通知给调用线程。
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        loop_ = std::make_unique<EventLoop>();
        if (initCallback_) {
            initCallback_(loop_.get());
        }
    }

    condition_.notify_one(); // 不放在锁内，逻辑更清晰，性能影响微乎其微。

    // 启动事件循环，直到 loop_->quit() 被调用。也就是说每一个线程里面的执行流都是由 loop_->loop() 驱动的
    loop_->loop();

    // loop 退出后再清空对外可见指针，避免其他线程读到悬空对象。
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        loop_.reset();
    }
}

void EventLoopThread::wait_for_loop() {
    std::unique_lock<std::mutex> lock(loopMutex_);
    condition_.wait(lock, [this]() { return loop_ != nullptr; });
}
