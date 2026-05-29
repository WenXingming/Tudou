// ============================================================================
// EventLoopThreadPool.cpp
// EventLoop 线程池实现，显式展开"创建主 loop、启动 IO 线程、轮询选择 loop"。
// ============================================================================

#include "tudou/reactor/EventLoopThreadPool.h"
#include "tudou/reactor/EventLoopThread.h"
#include "tudou/reactor/EventLoop.h"
#include <cassert>

EventLoopThreadPool::EventLoopThreadPool(const std::string& name, int numThreads, const ThreadInitCallback& cb) :
    mainLoop_(nullptr),
    ioLoopThreads_(),
    ioLoopsIndex_(0),
    name_(name),
    numThreads_(numThreads),
    started_(false),
    initCallback_(cb) {
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

    // 单线程模式下，main loop 兼任 IO loop，需要执行 initCallback。
    if (numThreads_ == 0 && initCallback_) {
        initCallback_(mainLoop_.get());
    }
}

void EventLoopThreadPool::create_io_threads() {
    for (int index = 0; index < numThreads_; ++index) {
        auto ioThread = std::make_unique<EventLoopThread>(initCallback_);
        ioThread->start();

        // 线程启动成功后再纳入池中，保证池内对象都能返回有效 loop。
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
