/**
 * @file EventLoopThreadPool.cpp
 * @brief 将多个 EventLoopThread 组合成线程池的封装类
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
*/

#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"
#include "EventLoop.h"
#include <cassert>

EventLoopThreadPool::EventLoopThreadPool(const std::string& name, int numThreads, const ThreadInitCallback& cb) :
    mainLoop_(nullptr),
    ioLoopThreads_(),
    ioLoopsIndex_(0),
    name_(name),
    numThreads_(numThreads),
    initCallback_(cb),
    started_(false) {

    mainLoop_.reset(new EventLoop());
    mainLoop_->assert_in_loop_thread();
}

void EventLoopThreadPool::start() {
    mainLoop_->assert_in_loop_thread();
    assert(!started_);

    for (int i = 0; i < numThreads_; ++i) {
        std::unique_ptr<EventLoopThread> ioThread(new EventLoopThread(initCallback_));
        ioThread->start();
        ioLoopThreads_.push_back(std::move(ioThread));
    }

    if (numThreads_ == 0 && initCallback_) {
        initCallback_(mainLoop_.get());
    }

    started_ = true;
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    mainLoop_->assert_in_loop_thread();
    assert(started_);

    // 默认轮询算法获取下一个 IO 线程的 EventLoop 指针
    EventLoop* loop = mainLoop_.get();
    if (!ioLoopThreads_.empty()) {
        loop = ioLoopThreads_[ioLoopsIndex_]->get_loop();
        // ++ioLoopsIndex_;
        // if (ioLoopsIndex_ >= ioLoopThreads_.size()) {
        //     ioLoopsIndex_ = 0;
        // }
        ioLoopsIndex_ = (ioLoopsIndex_ + 1) % ioLoopThreads_.size(); // 映射到 [0, size)
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::get_all_loops() const {
    assert(started_);
    std::vector<EventLoop*> loops;
    for (int i = 0; i < ioLoopThreads_.size(); ++i) {
        loops.push_back(ioLoopThreads_[i]->get_loop());
    }
    return std::move(loops);
}
