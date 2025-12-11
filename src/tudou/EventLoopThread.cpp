/**
 * @file EventLoopThread.h
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
    , initCallback(cb) /* 默认传参是空 std::function */ {

}

EventLoopThread::~EventLoopThread() {
    {
        std::lock_guard<std::mutex> lock(mtx); // 或者使用 std::unique_lock<std::mutex>
        isExiting = true;
    }
    // 退出 loop 和线程（保持同步）
    if (loop) {
        loop->quit();
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
        retLoop = loop;
    }
    return retLoop;
}

void EventLoopThread::thread_func() {
    // 创建该线程的 EventLoop 对象（思考：外部持有线程内部的 EventLoop 的指针是否安全？是否该使用智能指针 shared_ptr + tie）
    EventLoop eventLoop;
    if (initCallback) {
        initCallback(&eventLoop);
    }

    {
        std::lock_guard<std::mutex> lock(mtx);
        loop = &eventLoop;
        condition.notify_one();
    }

    eventLoop.loop();

    // loop 返回，代表 loop 退出
    {
        std::lock_guard<std::mutex> lock(mtx);
        loop = nullptr;
    }
}
