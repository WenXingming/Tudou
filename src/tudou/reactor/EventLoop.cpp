// ============================================================================
// EventLoop.cpp
// Reactor 核心循环实现，显式展开 poll、唤醒和任务执行的控制路径。
// ============================================================================

#include "tudou/reactor/EventLoop.h"

#include "tudou/reactor/EpollPoller.h"
#include "tudou/reactor/Channel.h"
#include "tudou/timer/TimerQueue.h"
#include "spdlog/spdlog.h"

#include <cassert>
#include <sys/eventfd.h>
#include <unistd.h>
#include <thread>

thread_local EventLoop* EventLoop::loopInThisThread = nullptr;
EventLoop::EventLoop(int pollTimeoutMs) :
    threadId_(std::this_thread::get_id()),
    pollTimeoutMs_(pollTimeoutMs),
    poller_(nullptr),
    isLooping_(false),
    isQuit_(false),
    pendingFunctors_(),
    pendingFunctorsMutex_(),
    isCallingPendingFunctors_(false),
    timerQueue_(nullptr) {

    // 先校验线程归属约束，再创建底层资源，避免在非法构造路径上先注册 fd。
    if (loopInThisThread != nullptr) {
        spdlog::critical("Cannot create more than one EventLoop per thread");
        assert(false);
    }
    loopInThisThread = this;
    poller_ = std::make_unique<EpollPoller>(this);

    // wakeupChannel_ 依赖 poller_ 完成注册，因此两者初始化顺序必须固定。
    wakeupFd_.reset(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (wakeupFd_.fd() < 0) {
        spdlog::critical("EventLoop: Failed to create eventfd");
        assert(false);
    }
    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_.fd());
    wakeupChannel_->set_read_callback([this](Channel&) { on_read(); });
    wakeupChannel_->enable_reading();

    timerQueue_ = std::make_unique<TimerQueue>(this);
}

EventLoop::~EventLoop() {
    if (loopInThisThread != this) {
        spdlog::error("EventLoop destructed in wrong thread");
        assert(false);
    }
    loopInThisThread = nullptr;
    // 成员按声明逆序自动析构：timerQueue_ → wakeupChannel_（epoll 注销）→ wakeupFd_（close fd）。
}

void EventLoop::loop() {
    assert(is_in_loop_thread());

    isLooping_ = true;
    while (!isQuit_) {
        const auto& activeChannels = poller_->poll(pollTimeoutMs_);

        for (Channel* channel : activeChannels) {
            channel->handle_events();
        }
        do_pending_functors();
    }
    isLooping_ = false;
}

void EventLoop::update_channel(Channel* channel) const {
    assert(is_in_loop_thread());
    poller_->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) const {
    assert(is_in_loop_thread());
    poller_->remove_channel(channel);
}

bool EventLoop::has_channel(Channel* channel) const {
    assert(is_in_loop_thread());
    return poller_->has_channel(channel);
}

void EventLoop::quit() {
    isQuit_ = true;
    // 如果调用 quit() 的线程不是 EventLoop 所在线程，必须通过 wakeup 唤醒它，让 loop() 能够及时感知 isQuit_ 的变化并退出。
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

bool EventLoop::is_in_loop_thread() const {
    return threadId_ == std::this_thread::get_id();
}

void EventLoop::run_in_loop(const Functor& cb) {
    if (!cb) {
        spdlog::error("EventLoop::run_in_loop() received empty functor");
        return;
    }

    if (is_in_loop_thread()) {
        cb();
        return;
    }
    queue_in_loop(cb);
}

void EventLoop::queue_in_loop(const Functor& cb) {
    if (!cb) {
        spdlog::error("EventLoop::queue_in_loop() received empty functor");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(pendingFunctorsMutex_);
        pendingFunctors_.push(cb);
    }

    // 跨线程投递和“执行队列期间再投递”都必须立即唤醒，避免任务拖到下一轮 poll。
    if (!is_in_loop_thread() || isCallingPendingFunctors_) {
        wakeup();
    }
}

TimerId EventLoop::run_at(std::chrono::steady_clock::time_point when, const Functor& cb) {
    return timerQueue_->add_timer(cb, when, std::chrono::milliseconds(0));
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

void EventLoop::wakeup() {
    const uint64_t one = 1;
    const ssize_t n = ::write(wakeupFd_.fd(), &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::wakeup() writes {} bytes instead of 8", n);
    }
}

void EventLoop::on_read() {
    uint64_t one = 1;
    const ssize_t n = ::read(wakeupFd_.fd(), &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::on_read() reads {} bytes instead of 8", n);
    }
}

void EventLoop::do_pending_functors() {
    // 任务执行路径固定为“摘队列 -> 顺序执行”，避免锁与回调逻辑交织。
    FunctorQueue functors;
    isCallingPendingFunctors_ = true;
    {
        std::lock_guard<std::mutex> lock(pendingFunctorsMutex_);
        functors.swap(pendingFunctors_);
    }

    while (!functors.empty()) {
        // 这里必须做值拷贝；front() 返回的引用在 pop() 后会立即悬空。
        Functor functor = std::move(functors.front());
        functors.pop();
        functor();
    }
    isCallingPendingFunctors_ = false;
}
