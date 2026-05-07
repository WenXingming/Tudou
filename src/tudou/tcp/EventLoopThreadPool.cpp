// ============================================================================
// EventLoopThreadPool.cpp
// EventLoop 线程池实现，显式展开“创建主 loop、启动 IO 线程、轮询选择 loop”。
// ============================================================================

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

EventLoopThreadPool::~EventLoopThreadPool() = default;

void EventLoopThreadPool::start() {
    mainLoop_->assert_in_loop_thread();
    assert(!started_);

    // 先把后台 IO loops 启起来，后续 accept 到的连接才能立即分发出去。
    create_io_threads();
    initialize_main_loop_if_needed();
    started_ = true;
}

void EventLoopThreadPool::create_io_threads() {
    for (int index = 0; index < numThreads_; ++index) {
        auto ioThread = std::make_unique<EventLoopThread>(initCallback_);
        ioThread->start();

        // 线程启动成功后再纳入池中，保证池内对象都能返回有效 loop。
        ioLoopThreads_.push_back(std::move(ioThread));
    }
}

void EventLoopThreadPool::initialize_main_loop_if_needed() const {
    if (numThreads_ == 0 && initCallback_) {
        initCallback_(mainLoop_.get());
    }
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    mainLoop_->assert_in_loop_thread();
    assert(started_);

    EventLoop* loop = mainLoop_.get();
    if (!ioLoopThreads_.empty()) {
        // 连接分发采用 round-robin，保持不同 IO loop 的负载大体均衡。
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
