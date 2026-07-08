// ============================================================================
// EventLoopThreadPool.cpp
// EventLoop 线程池实现，显式展开"创建主 loop、启动 IO 线程、轮询选择 loop"。
// ============================================================================

#include "tudou/reactor/EventLoopThreadPool.h"
#include "tudou/reactor/EventLoopThread.h"
#include "tudou/reactor/EventLoop.h"
#include "spdlog/spdlog.h"
#include <cassert>
#include <pthread.h>
#include <sched.h>
#include <thread>

EventLoopThreadPool::EventLoopThreadPool(const std::string& name, int numThreads, const ThreadInitCallback& cb, bool pinCpu) :
    mainLoop_(nullptr),
    ioLoopThreads_(),
    ioLoopsIndex_(0),
    name_(name),
    numThreads_(numThreads),
    started_(false),
    initCallback_(cb),
    pinCpu_(pinCpu) {
}

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start() {
    if (started_) {
        return;
    }

    create_main_loop();
    create_io_threads();

    started_ = true;
}

void EventLoopThreadPool::create_main_loop() {
    // 在当前线程创建主 EventLoop（也就是说主 loop 跑在调用 start() 的线程上）
    mainLoop_ = std::make_unique<EventLoop>();

    if (pinCpu_) {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(0, &cpuset); // 主线程固定绑定到 CPU 核心 0
        pthread_t currentThread = ::pthread_self();
        if (::pthread_setaffinity_np(currentThread, sizeof(cpu_set_t), &cpuset) != 0) {
            spdlog::warn("EventLoopThreadPool: Failed to bind main thread to CPU core 0");
        } else {
            spdlog::info("EventLoopThreadPool: Successfully bound main thread to CPU core 0");
        }
    }

    // 单线程模式下，main loop 兼任 IO loop，需要执行 initCallback。
    if (numThreads_ == 0 && initCallback_) {
        initCallback_(mainLoop_.get());
    }
}

void EventLoopThreadPool::create_io_threads() {
    int numCores = static_cast<int>(std::thread::hardware_concurrency());
    for (int index = 0; index < numThreads_; ++index) {
        int cpuCore = -1;
        if (pinCpu_ && numCores > 0) {
            // IO 线程依次绑定到 Core 1, 2, ...
            cpuCore = (index + 1) % numCores;
        }
        auto ioThread = std::make_unique<EventLoopThread>(initCallback_, cpuCore);
        ioLoopThreads_.push_back(std::move(ioThread));
    }
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    assert(mainLoop_->is_in_loop_thread());
    assert(started_);

    // 连接分发采用 round-robin，保持不同 IO loop 的负载大体均衡。
    EventLoop* loop = mainLoop_.get();
    if (!ioLoopThreads_.empty()) {
        loop = ioLoopThreads_[ioLoopsIndex_]->get_loop();
        ioLoopsIndex_ = (ioLoopsIndex_ + 1) % ioLoopThreads_.size();
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::get_all_loops() const {
    assert(started_);

    std::vector<EventLoop*> loops;
    loops.reserve(ioLoopThreads_.size() + 1);
    loops.push_back(mainLoop_.get());
    for (const auto& ioLoopThread : ioLoopThreads_) {
        loops.push_back(ioLoopThread->get_loop());
    }
    return loops;
}
