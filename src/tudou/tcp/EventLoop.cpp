// ============================================================================
// EventLoop.cpp
// Reactor 核心循环实现，显式展开 poll、唤醒和任务执行的控制路径。
// ============================================================================

#include "EventLoop.h"

#include "EpollPoller.h"
#include "Channel.h"
#include "TimerQueue.h"
#include "spdlog/spdlog.h"

#include <cassert>
#include <sys/eventfd.h>
#include <unistd.h>
#include <thread>

thread_local EventLoop* EventLoop::loopInthisThread = nullptr;
const int EventLoop::pollTimeoutMs_ = 10000;

EventLoop::EventLoop() :
    poller_(std::make_unique<EpollPoller>(this)),
    isLooping_(false),
    isQuit_(false),
    threadId_(std::this_thread::get_id()),
    wakeupFd_(-1),
    wakeupChannel_(nullptr),
    pendingFunctors_(),
    isCallingPendingFunctors_(false),
    mtx_(),
    timerQueue_(nullptr) {

    // wakeupChannel_ 依赖 poller_ 完成注册，因此两者初始化顺序必须固定。
    wakeupFd_ = create_wakeup_fd();
    if (wakeupFd_ < 0) {
        spdlog::critical("EventLoop: Failed to create wakeupFd");
        assert(false);
    }
    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupChannel_->set_read_callback([this](Channel&) { on_read(); });
    wakeupChannel_->enable_reading();

    timerQueue_ = std::make_unique<TimerQueue>(this);

    if (loopInthisThread != nullptr) {
        spdlog::critical("Cannot create more than one EventLoop per thread");
        assert(false);
    }
    loopInthisThread = this;
}

EventLoop::~EventLoop() {
    if (loopInthisThread != this) {
        spdlog::error("EventLoop destructed in wrong thread");
        assert(false);
    }
    loopInthisThread = nullptr;
}

void EventLoop::loop(int timeoutMs) {
    assert_in_loop_thread();
    isQuit_ = false;
    isLooping_ = true;

    while (!isQuit_) {
        // 每轮循环只做两件事：处理内核事件，然后处理本轮排队任务。
        poller_->poll(timeoutMs);
        do_pending_functors();
    }

    isLooping_ = false;
}

void EventLoop::update_channel(Channel* channel) const {
    assert_in_loop_thread();
    poller_->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) const {
    assert_in_loop_thread();
    poller_->remove_channel(channel);
}

bool EventLoop::has_channel(Channel* channel) const {
    assert_in_loop_thread();
    return poller_->has_channel(channel);
}

void EventLoop::quit() {
    isQuit_ = true;
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

bool EventLoop::is_in_loop_thread() const {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::assert_in_loop_thread() const {
    assert(is_in_loop_thread());
}

void EventLoop::run_in_loop(const Functor& cb) {
    if (is_in_loop_thread()) {
        cb();
        return;
    }
    queue_in_loop(cb);
}

void EventLoop::queue_in_loop(const Functor& cb) {
    {
        std::unique_lock<std::mutex> lock(mtx_);
        pendingFunctors_.push(cb);
    }

    // 跨线程投递和“执行队列期间再投递”都必须立即唤醒，避免任务拖到下一轮 poll。
    if (!is_in_loop_thread() || isCallingPendingFunctors_) {
        wakeup();
    }
}

TimerId EventLoop::run_after(double delaySeconds, const Functor& cb) {
    auto delay = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(delaySeconds));
    if (delay.count() < 0) {
        delay = std::chrono::milliseconds(0);
    }
    auto when = std::chrono::steady_clock::now() + delay;
    return timerQueue_->add_timer(cb, when, std::chrono::milliseconds(0));
}

TimerId EventLoop::run_every(double intervalSeconds, const Functor& cb) {
    auto interval = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::duration<double>(intervalSeconds));
    if (interval.count() <= 0) {
        interval = std::chrono::milliseconds(1);
    }
    auto when = std::chrono::steady_clock::now() + interval;
    return timerQueue_->add_timer(cb, when, interval);
}

void EventLoop::cancel(TimerId timerId) {
    if (!timerId.valid()) {
        return;
    }
    timerQueue_->erase_timer(timerId);
}

int EventLoop::create_wakeup_fd() {
    int eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd < 0) {
        spdlog::error("Failed to create eventfd");
        assert(false);
    }
    return eventFd;
}

void EventLoop::wakeup() {
    const uint64_t one = 1;
    const ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::wakeup() writes {} bytes instead of 8", n);
    }
}

void EventLoop::on_read() {
    uint64_t one = 1;
    const ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::on_read() reads {} bytes instead of 8", n);
    }
}

void EventLoop::do_pending_functors() {
    // 任务执行路径固定为“摘队列 -> 顺序执行”，避免锁与回调逻辑交织。
    FunctorQueue functors = take_pending_functors();
    execute_pending_functors(functors);
}

EventLoop::FunctorQueue EventLoop::take_pending_functors() {
    FunctorQueue functors;
    isCallingPendingFunctors_ = true;
    {
        std::unique_lock<std::mutex> lock(mtx_);
        functors.swap(pendingFunctors_);
    }
    return functors;
}

void EventLoop::execute_pending_functors(FunctorQueue& functors) {
    while (!functors.empty()) {
        // 这里必须做值拷贝；front() 返回的引用在 pop() 后会立即悬空。
        Functor functor = functors.front();
        functors.pop();
        functor();
    }
    isCallingPendingFunctors_ = false;
}