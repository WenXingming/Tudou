#include "EventLoop.h"
#include "Channel.h"
#include "spdlog/spdlog.h"

#include <thread>
#include <functional>

// one loop per thread。实现方式是使用 thread_local 关键字作为线程局部存储的标记即可
thread_local EventLoop* loopInthisThread = nullptr;

EventLoop::EventLoop()
    : poller(new EpollPoller())
    , isLooping(true) {

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
}

bool EventLoop::get_is_looping() const {
    return isLooping;
}

void EventLoop::set_is_looping(bool looping) {
    isLooping = looping;
}

void EventLoop::loop(int timeoutMs) {
    spdlog::info("EventLoop start looping...");

    poller->set_poll_timeout_ms(timeoutMs);
    while (isLooping) {
        do_pending_functors();

        std::hash<std::thread::id> hasher;
        size_t threadId = hasher(std::this_thread::get_id());
        spdlog::info("Thread hash: {}", threadId);

        // 使用 poller 监听发生事件的 channels
        poller->poll();
    }

    spdlog::info("EventLoop stop looping.");
}

void EventLoop::update_channel(Channel* channel) const {
    poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) const {
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
    std::lock_guard<std::mutex> lock(mtx);
    pendingFunctors.push(cb);
}

void EventLoop::do_pending_functors() {
    std::queue<std::function<void()>> functors;

    // 将待执行的函数交换到本地变量，减少锁的持有时间
    {
        std::lock_guard<std::mutex> lock(mtx);
        functors.swap(pendingFunctors);
    }

    while (!functors.empty()) {
        auto& func = functors.front();
        func();
        functors.pop();
    }
}