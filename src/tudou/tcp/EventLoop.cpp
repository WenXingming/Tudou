/**
 * @file EventLoop.cpp
 * @brief 事件循环（Reactor）核心类的实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "EventLoop.h"
#include "EpollPoller.h"
#include "Channel.h"
#include "spdlog/spdlog.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <thread>
#include <functional>

thread_local EventLoop* EventLoop::loopInthisThread = nullptr; // one loop per thread。防止一个线程创建多个 EventLoop 实例，实现方式是使用 thread_local 关键字作为线程局部存储的标记即可
const int EventLoop::pollTimeoutMs_ = 10000;

EventLoop::EventLoop() :
    poller_(std::make_unique<EpollPoller>(this)), // C++14
    isLooping_(false),
    isQuit_(false),
    threadId_(std::this_thread::get_id()),
    wakeupFd_(-1),
    wakeupChannel_(nullptr),
    pendingFunctors_(),
    isCallingPendingFunctors_(false),
    mtx_() {

    // 创建 wakeupFd 和 wakeupChannel（注意：poller_ 必须先于 wakeupChannel_ 初始化）
    wakeupFd_ = create_wakeup_fd();
    if (wakeupFd_ < 0) {
        spdlog::critical("EventLoop: Failed to create wakeupFd");
        assert(false);
    }
    wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_);
    wakeupChannel_->set_read_callback([this](Channel&) { on_read(); });
    wakeupChannel_->enable_reading();

    // one loop per thread 保证
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

void EventLoop::run_in_loop(const std::function<void()>& cb) {
    if (is_in_loop_thread()) {
        cb();
        return;
    }
    queue_in_loop(cb);
}

void EventLoop::queue_in_loop(const std::function<void()>& cb) {
    {
        std::unique_lock<std::mutex> lock(mtx_);
        pendingFunctors_.push(cb);
    }
    // 非 loop 线程或正在执行 pendingFunctors 时唤醒，避免新 functor 被延迟到下轮 poll
    if (!is_in_loop_thread() || isCallingPendingFunctors_) {
        wakeup();
    }
}

int EventLoop::create_wakeup_fd() {
    // eventfd 是 Linux 提供的一个轻量级的事件通知机制，适合用于线程间通知
    int eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd < 0) {
        spdlog::error("Failed to create eventfd");
        assert(false);
    }
    return eventFd;
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::wakeup() writes {} bytes instead of 8", n);
    }
}

void EventLoop::on_read() {
    uint64_t one = 1;
    ssize_t n = ::read(wakeupFd_, &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::on_read() reads {} bytes instead of 8", n);
    }
}

void EventLoop::do_pending_functors() {
    // 将待执行的函数交换到本地局部变量，减少锁的持有时间（不能把执行回调的过程放在锁内）
    std::queue<std::function<void()>> functors;
    isCallingPendingFunctors_ = true;
    {
        std::unique_lock<std::mutex> lock(mtx_);
        functors.swap(pendingFunctors_);
    }
    while (!functors.empty()) {
        auto functor = functors.front(); // 必须值拷贝，不能用引用！pop 后引用悬空 (see docs/Document.md#pending-functors-bug)
        functors.pop();
        functor();
    }
    isCallingPendingFunctors_ = false;
}