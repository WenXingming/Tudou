// ============================================================================
// EventLoopThread.cpp
// EventLoop 线程绑定器实现。
// ============================================================================

#include "tudou/reactor/EventLoopThread.h"

#include "tudou/reactor/EventLoop.h"
#include "EventLoopThread.h"
#include "spdlog/spdlog.h"

#include <pthread.h>
#include <sched.h>

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb, int cpuCore)
    : loop_(nullptr)
    , thread_()
    , loopMutex_()
    , loopCondition_()
    , initCallback_(cb)
    , cpuCore_(cpuCore) {

    // 启动后台线程，创建 EventLoop 并阻塞等待就绪。
    thread_ = std::thread(&EventLoopThread::thread_func, this);
    std::unique_lock<std::mutex> lock(loopMutex_);
    loopCondition_.wait(lock, [this]() { return loop_ != nullptr; });
}

EventLoopThread::~EventLoopThread() {
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        if (loop_) {
            loop_->quit();
        }
    }
    if (thread_.joinable()) {
        thread_.join(); // std::thread::join() 是同步屏障——调用方会阻塞，直到 thread_func() 完整返回后 join() 才返回。这是 C++ 标准保证的。不会产生 loop_ 未被 reset
    }
}


EventLoop * EventLoopThread::get_loop(){
    std::lock_guard<std::mutex> lock(loopMutex_);
    return loop_.get(); 
}

void EventLoopThread::thread_func() {
    // 绑定 CPU 亲和性
    if (cpuCore_ >= 0) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpuCore_, &cpuset);
        pthread_t currentThread = ::pthread_self();
        if (::pthread_setaffinity_np(currentThread, sizeof(cpu_set_t), &cpuset) != 0) {
            spdlog::warn("EventLoopThread: Failed to set CPU affinity to core {}", cpuCore_);
        } else {
            spdlog::info("EventLoopThread: Successfully bound thread to CPU core {}", cpuCore_);
        }
    }

    // 创建该线程专属的 EventLoop，执行初始化回调，然后通知构造函数可以返回。
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        loop_ = std::make_unique<EventLoop>();
        if (initCallback_) {
            initCallback_(loop_.get());
        }
    }
    loopCondition_.notify_one();

    // 启动事件循环，直到 loop_->quit() 被调用。
    loop_->loop();

    // loop 退出后再清空对外可见指针，避免其他线程读到悬空对象。
    {
        std::lock_guard<std::mutex> lock(loopMutex_);
        loop_.reset();
    }
}