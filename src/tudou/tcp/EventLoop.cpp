/**
 * @file EventLoop.cpp
 * @brief 事件循环（Reactor）核心类的实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "EventLoop.h"
#include "Channel.h"
#include "spdlog/spdlog.h"
#include <sys/eventfd.h>
#include <unistd.h>
#include <thread>
#include <functional>

thread_local EventLoop* EventLoop::loopInthisThread = nullptr; // one loop per thread。防止一个线程创建多个 EventLoop 实例，实现方式是使用 thread_local 关键字作为线程局部存储的标记即可.创建一个 EventLoop 实例时，检查该线程的 loopInthisThread 

const int EventLoop::pollTimeoutMs_ = 10000; // epoll_wait 超时时间，单位毫秒

EventLoop::EventLoop() :
    poller_(new EpollPoller(this)),
    isLooping_(false),
    isQuit_(false),
    threadId_(std::this_thread::get_id()),
    wakeupFd_(-1),
    wakeupChannel_(nullptr),
    pendingFunctors_(),
    isCallingPendingFunctors_(false),
    mtx_() {

    // 设置 wakeupChannel_ 的读事件回调函数。每一个 EventLoop 都有一个 wakeupChannel，用于唤醒 loop 线程
    // 创建 channel 时会将 channel 注册到 poller_ 上。这里有一个细节，构造函数里先创建了 poller，再创建 wakeupChannel_
    wakeupFd_ = this->create_wakeup_fd();
    if (wakeupFd_ < 0) {
        spdlog::critical("EventLoop::EventLoop(): Failed to create wakeupFd_");
        assert(false);
    }
    wakeupChannel_.reset(new Channel(this, wakeupFd_));
    wakeupChannel_->set_read_callback(std::bind(&EventLoop::on_read, this));
    wakeupChannel_->enable_reading(); // 关注读事件

    // 确保每个线程只能有一个 EventLoop 实例
    if (loopInthisThread != nullptr) {
        spdlog::critical("Can not create more than one EventLoop in a thread");
        assert(false);
    }
    loopInthisThread = this;
    spdlog::debug("EventLoop created in a thread.");
}

EventLoop::~EventLoop() {
    // 原 muduo 还在这里管理了 wakeupChannel_ 的析构时的一些处理，感觉并不合理。我放在了 Channel 的析构函数中处理
    if (loopInthisThread != this) {
        spdlog::error("EventLoop destructed in wrong thread.");
        assert(false);
    }
    loopInthisThread = nullptr;
    spdlog::debug("EventLoop destructed in its thread.");
}

void EventLoop::loop(int timeoutMs) {
    spdlog::debug("EventLoop start looping...");
    assert_in_loop_thread();
    isQuit_ = false;

    isLooping_ = true;
    while (!isQuit_) {
        poller_->poll(pollTimeoutMs_); // 使用 poller_ 监听发生事件的 channels，包括 wakeupChannel_

        do_pending_functors(); // 处理需要在当前线程执行的函数。实际上是 wakeupChannel_ 的真正需要执行的逻辑，前面的 on_read 只是读取 wakeupFd，清除事件
    }
    isLooping_ = false;

    spdlog::debug("EventLoop stop looping.");
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

    // 如果不是在 loop 所在线程调用，则需要唤醒 loop 线程进行退出
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

bool EventLoop::is_in_loop_thread() const {
    return threadId_ == std::this_thread::get_id(); // 优化：使用一个 thread_local 变量存储当前线程 id，避免每次断言（is_in_loop_thread()）调用都获取线程 id。但 std::this_thread::get_id() 本身开销也不大，无需通过缓存进行优化
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
    // 可能会被其他线程调用，需要加锁保护 pendingFunctors_
    {
        std::unique_lock<std::mutex> lock(mtx_);
        pendingFunctors_.push(cb);
    }

    // 如果不是在 loop 所在线程调用，或者当前正在调用 pending functors（当前又加入了新的 functor，其并不会在本轮执行），则需要唤醒 loop 线程。isCallingPendingFunctors 的作用：
    // 用于避免当前 pending functors 执行完后，阻塞在下一轮 poll 上，无法及时处理新加入的 functor。即其作用是预防未来可能的阻塞（因为肯定有 pending functors 待执行：当前新加入的）
    if (!is_in_loop_thread() || isCallingPendingFunctors_) {
        wakeup();
    }
}

int EventLoop::create_wakeup_fd() {
    // 创建 wakeupFd，用于多线程间 notify，唤醒 loop 线程
    int eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd < 0) {
        spdlog::error("Failed in eventfd creation");
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
        auto functor = functors.front(); // 不能是引用!!! 找了半天的多线程 bug。现象是 lambda 表达式里访问的变量值总是错误的，后来发现是这里的引用导致的悬空引用
        functors.pop();
        functor();
    }
    isCallingPendingFunctors_ = false;
}