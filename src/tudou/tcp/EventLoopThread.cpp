// ============================================================================
// EventLoopThread.cpp
// EventLoop 线程绑定器实现，显式展开“起线程、发布 loop、驱动 loop、清理”。
// ============================================================================

#include "EventLoopThread.h"

#include "EventLoop.h"

#include <cassert>

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb)
    : loop_(nullptr)
    , thread_(nullptr)
    , mtx_()
    , condition_()
    , initCallback_(cb)
    , started_(false) {

}

EventLoopThread::~EventLoopThread() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
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
    assert(!started_);
    launch_thread();
    wait_until_loop_ready();
    started_ = true;
}

void EventLoopThread::launch_thread() {
    thread_ = std::make_unique<std::thread>(&EventLoopThread::thread_func, this);
}

void EventLoopThread::wait_until_loop_ready() {
    std::unique_lock<std::mutex> lock(mtx_);
    while (loop_ == nullptr) {
        condition_.wait(lock);
    }
}

void EventLoopThread::thread_func() {
    std::unique_ptr<EventLoop> eventLoop = create_loop();
    initialize_loop(eventLoop.get());

    // 先对外发布 loop_，再进入 loop()，这样启动方拿到的一定是可用对象。
    publish_loop(std::move(eventLoop));
    loop_->loop();

    // loop 退出后再清空对外可见指针，避免其他线程读到悬空对象。
    clear_loop();
}

std::unique_ptr<EventLoop> EventLoopThread::create_loop() const {
    return std::make_unique<EventLoop>();
}

void EventLoopThread::initialize_loop(EventLoop* loop) const {
    if (initCallback_) {
        initCallback_(loop);
    }
}

void EventLoopThread::publish_loop(std::unique_ptr<EventLoop> loop) {
    std::lock_guard<std::mutex> lock(mtx_);
    loop_ = std::move(loop);
    condition_.notify_one();
}

void EventLoopThread::clear_loop() {
    std::lock_guard<std::mutex> lock(mtx_);
    loop_.reset();
}
