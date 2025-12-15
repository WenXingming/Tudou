/**
 * @file EpollPoller.h
 * @brief 基于 epoll 的 Poller 实现 — 多路 I/O 事件监听、分发器（Reactor 的 I/O 多路复用层）
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

 // one loop per thread。防止一个线程创建多个 EventLoop 实例，实现方式是使用 thread_local 关键字作为线程局部存储的标记即可
thread_local EventLoop* loopInthisThread = nullptr;

const int EventLoop::pollTimeoutMs = 10000;

// 创建 wakeupFd，用于多线程间 notify，唤醒 loop 线程
int create_event_fd() {
    int eventFd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (eventFd < 0) {
        spdlog::error("Failed in eventfd creation");
        assert(false);
    }
    return eventFd;
}

EventLoop::EventLoop()
    : poller(new EpollPoller(this))
    , isLooping(true)
    , isQuit(false)
    , threadId(std::this_thread::get_id())
    , wakeupFd(::create_event_fd())
    , wakeupChannel(new Channel(this, wakeupFd))
    , isCallingPendingFunctors(false)
    , pendingFunctors()
    , mtx() {

    std::hash<std::thread::id> hasher;
    size_t threadId = hasher(std::this_thread::get_id());
    // 确保每个线程只能有一个 EventLoop 实例
    if (loopInthisThread != nullptr) {
        spdlog::error("Another EventLoop exists in this thread: {}", threadId);
        assert(false);
    }
    else {
        loopInthisThread = this;
        spdlog::info("EventLoop created in thread: {}", threadId);
    }

    // 设置 wakeupChannel 的读事件回调函数。每一个 EventLoop 都有一个 wakeupChannel，用于唤醒 loop 线程
    // 创建 channel 时会将 channel 注册到 poller 上。这里有一个细节，构造函数里先创建了 poller，再创建 wakeupChannel
    wakeupChannel->set_read_callback(std::bind(&EventLoop::on_read, this));
    wakeupChannel->enable_reading(); // 关注读事件
}

EventLoop::~EventLoop() {
    // 原 muduo 还在这里管理了 wakeupChannel 的析构，感觉并不合理。我放在了 Channel 的析构函数中处理

    if (loopInthisThread == this) {
        loopInthisThread = nullptr;

        std::hash<std::thread::id> hasher;
        size_t threadId = hasher(std::this_thread::get_id());
        spdlog::info("EventLoop destroyed in thread: {}", threadId);
    }
    else {
        spdlog::error("EventLoop destroyed in wrong thread.");
        assert(false);
    }
}

void EventLoop::loop(int timeoutMs) {
    spdlog::info("EventLoop start looping...");

    assert_in_loop_thread();
    isQuit = false;
    isLooping = true;
    while (!isQuit) {
        assert_in_loop_thread();

        // 使用 poller 监听发生事件的 channels，包括 wakeupChannel
        poller->poll(pollTimeoutMs);

        // 处理需要在当前线程执行的函数。实际上是 wakeupChannel 的真正需要执行的逻辑，前面的 on_read 只是读取 wakeupFd，清除事件
        do_pending_functors();
    }
    isLooping = false;

    spdlog::info("EventLoop stop looping.");
}

void EventLoop::quit() {
    isQuit = true;

    // 如果不是在 loop 所在线程调用，则需要唤醒 loop 线程进行退出
    if (!is_in_loop_thread()) {
        wakeup();
    }
}

bool EventLoop::has_channel(Channel* channel) const {
    assert_in_loop_thread();
    return poller->has_channel(channel);
}

void EventLoop::update_channel(Channel* channel) const {
    assert_in_loop_thread();
    poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) const {
    assert_in_loop_thread();
    poller->remove_channel(channel);
}

void EventLoop::assert_in_loop_thread() const {
    // assert(loopInthisThread == this);
    assert(is_in_loop_thread());
}

bool EventLoop::is_in_loop_thread() const {
    // return loopInthisThread == this;
    return std::this_thread::get_id() == threadId;
}

void EventLoop::run_in_loop(const std::function<void()>& cb) {
    if (is_in_loop_thread()) {
        cb();
    }
    else {
        queue_in_loop(cb);
    }
}

void EventLoop::queue_in_loop(const std::function<void()>& cb) {
    {
        std::unique_lock<std::mutex> lock(mtx);
        pendingFunctors.push(cb);
    }
    // 如果不是在 loop 所在线程调用，或者当前正在调用 pending functors（当前又加入了新的 functor，其并不会在本轮执行），则需要唤醒 loop 线程。isCallingPendingFunctors 的作用：
    // 用于避免当前 pending functors 执行完后，阻塞在下一轮 poll 上，无法及时处理新加入的 functor。即其作用是预防未来可能的阻塞（因为肯定有 pending functors 待执行：当前新加入的）
    if (!is_in_loop_thread() || isCallingPendingFunctors) {
        wakeup();
    }
}

void EventLoop::wakeup() {
    uint64_t one = 1;
    ssize_t n = ::write(wakeupFd, &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::wakeup() writes {} bytes instead of 8", n);
    }
}

void EventLoop::on_read() {
    uint64_t one = 1;
    ssize_t n = ::read(wakeupFd, &one, sizeof(one));
    if (n != sizeof(one)) {
        spdlog::error("EventLoop::on_read() reads {} bytes instead of 8", n);
    }
}

void EventLoop::do_pending_functors() {
    // 将待执行的函数交换到本地局部变量，减少锁的持有时间（不能把执行回调的过程放在锁内）
    std::queue<std::function<void()>> functors;
    isCallingPendingFunctors = true;
    {
        std::unique_lock<std::mutex> lock(mtx);
        functors.swap(pendingFunctors);
    }

    while (!functors.empty()) {
        // auto& functor = functors.front(); // !!! 找了半天的 bug。现象是 lambda 表达式里访问的变量值总是错误的，后来发现是这里的引用导致的悬空引用
        auto functor = functors.front();
        functors.pop();
        functor();
    }
    isCallingPendingFunctors = false;
}