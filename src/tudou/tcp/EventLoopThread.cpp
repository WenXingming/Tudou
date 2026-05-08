// ============================================================================
// EventLoopThread.cpp
// EventLoop 线程绑定器实现。
// ============================================================================

#include "EventLoopThread.h"

#include "EventLoop.h"

#include <cassert>

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb)
    : loop_(nullptr)
    , thread_(nullptr)
    , loopMutex_()
    , condition_()
    , started_(false)
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

    started_ = true;
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

    // 我们是在 initCallback_ 后通知，也可以在 loop_ 创建完成后立即通知，甚至直接在 thread_func 内部 condition_.notify_one()，只要保证在 loop_ 创建完成后通知即可。
    signal_loop_ready();

    // 启动事件循环，直到 loop_->quit() 被调用。也就是说每一个线程里面的执行流都是由 loop_->loop() 驱动的
    loop_->loop();

    // loop 退出后再清空对外可见指针，避免其他线程读到悬空对象。
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        loop_.reset();
    }
}

void EventLoopThread::signal_loop_ready() {
    // condition_.notify_one() 本身不需要持锁——C++ 标准明确允许不带锁调用。
    // 锁的作用是保护共享状态 loop_，而 loop_ 已经在 thread_func 前面的锁作用域中写完了。这个锁是多余的，可以直接去掉 signal_loop_ready，内联为一行。
    std::lock_guard<std::mutex> lock(loopMutex_);
    condition_.notify_one();
}

void EventLoopThread::wait_for_loop() {
    std::unique_lock<std::mutex> lock(loopMutex_);
    while (loop_ == nullptr) {
        condition_.wait(lock);
    }
}
