#include "EventLoop.h"
#include "Channel.h"
#include "spdlog/spdlog.h"

#include <thread>
#include <functional>

// one loop per thread
// 实现方式是使用 thread_local 关键字作为线程局部存储的标记即可
thread_local EventLoop* loopInthisThread = nullptr;

EventLoop::EventLoop()
    : poller(new EpollPoller())
    , isLooping(true)
    , isQuit(false) {

    // 确保每个线程只能有一个 EventLoop 实例
    if (loopInthisThread != nullptr) {
        std::hash<std::thread::id> hasher;
        size_t threadId = hasher(std::this_thread::get_id());
        spdlog::error("Another EventLoop exists in this thread: {}", threadId);
        assert(false);
    }
    else {
        loopInthisThread = this;

        std::hash<std::thread::id> hasher;
        size_t threadId = hasher(std::this_thread::get_id());
        spdlog::info("EventLoop created in thread: {}", threadId);
    }
}

EventLoop::~EventLoop() {
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

void EventLoop::quit() {
    isQuit = true;

    /// TODO: 后续加入唤醒机制，唤醒 loop 线程
    if (!is_in_loop_thread()) {
        // wake up loop thread
    }
}

void EventLoop::loop(int timeoutMs) {
    spdlog::info("EventLoop start looping...");
    isQuit = false;

    isLooping = true;
    while (!isQuit) {
        assert_in_loop_thread();

        // 使用 poller 监听发生事件的 channels
        poller->set_poll_timeout_ms(timeoutMs);
        poller->poll();

        // 处理需要在当前线程执行的函数
        do_pending_functors();
    }
    isLooping = false;

    spdlog::info("EventLoop stop looping.");
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

void EventLoop::run_in_loop(const std::function<void()>& cb) {
    if (is_in_loop_thread()) {
        cb();
    }
    else {
        queue_in_loop(cb);
    }
}

void EventLoop::assert_in_loop_thread() const {
    assert(loopInthisThread == this);
}

bool EventLoop::is_in_loop_thread() const {
    return loopInthisThread == this;
}

void EventLoop::queue_in_loop(const std::function<void()>& cb) {
    {
        std::lock_guard<std::mutex> lock(mtx);
        pendingFunctors.push(cb);
    }
    /// TODO: 后续加入唤醒机制，唤醒 loop 线程
}

void EventLoop::do_pending_functors() {
    // 将待执行的函数交换到本地变量，减少锁的持有时间
    std::queue<std::function<void()>> functors;
    {
        std::lock_guard<std::mutex> lock(mtx);
        functors.swap(pendingFunctors);
    }

    while (!functors.empty()) {
        auto& func = functors.front();
        functors.pop();
        func();
    }
}