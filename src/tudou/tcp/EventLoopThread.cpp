/**
 * @file EventLoopThread.cpp
 * @brief 将 EventLoop 和 线程绑定的封装类
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb)
    : loop(nullptr)
    , thread(nullptr) // 默认初始化为空指针
    , mtx()
    , condition()
    , isExiting(false)
    , initCallback(cb) {

}

EventLoopThread::~EventLoopThread() {
    EventLoop* loopToQuit = nullptr;
    {
        std::lock_guard<std::mutex> lock(mtx); // 或者使用 std::unique_lock<std::mutex>
        isExiting = true;
        loopToQuit = loop.get();
    }
    // 退出 loop 和线程（保持同步）
    if (loopToQuit) {
        loopToQuit->quit();
    }
    if (thread && thread->joinable()) {
        thread->join();
    }
}

EventLoop* EventLoopThread::start_loop() {
    // 启动线程，线程执行 thread_func 函数
    thread.reset(new std::thread(&EventLoopThread::thread_func, this));

    EventLoop* retLoop = nullptr;
    {
        std::unique_lock<std::mutex> lock(mtx);
        while (loop == nullptr) { // 防止虚假唤醒
            condition.wait(lock); // 只能使用 unique_lock，因为 condition_variable 的 wait 需要释放锁
        }
        retLoop = loop.get();
    }
    return retLoop;
}

void EventLoopThread::thread_func() {
    // 在所属线程内创建 EventLoop，并通过 unique_ptr 明确其所有权。
    std::unique_ptr<EventLoop> eventLoop(new EventLoop());
    if (initCallback) {
        initCallback(eventLoop.get());
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        loop = std::move(eventLoop);
        condition.notify_one();
    }

    loop->loop();

    // loop 退出
    {
        std::lock_guard<std::mutex> lock(mtx);
        loop.reset();
    }
}
