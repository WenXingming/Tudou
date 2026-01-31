/**
 * @file EventLoopThread.h
 * @brief 将 EventLoop 和 线程绑定的封装类
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * - 没有按照 muduo 那样将 pthread 封装成类（也没有对 std::thread 进行二次封装），而是直接使用 std::thread
 * - 锁、条件变量什么的都是使用 C++11 标准库提供的 std::mutex、std::condition_variable 等
 */

#pragma once

#include <memory>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>

class EventLoop;
class EventLoopThread {
    using ThreadInitCallback = std::function<void(EventLoop*)>; // 线程创建后可以调用该回调函数进行一些初始化操作，如果未传入，则不进行任何操作

private:
    std::unique_ptr<EventLoop> loop; // 线程内创建并持有 EventLoop（所有权清晰，避免悬空指针）
    std::unique_ptr<std::thread> thread; // 不直接使用 std::thread，而是使用智能指针进行管理，方便控制线程的启动时机（和销毁）

    std::mutex mtx;
    std::condition_variable condition;

    ThreadInitCallback initCallback; // 线程初始化回调函数

public:
    EventLoopThread(const ThreadInitCallback& cb = ThreadInitCallback()); // 默认参数为空的 std::function
    EventLoopThread(const EventLoopThread&) = delete;
    EventLoopThread& operator=(const EventLoopThread&) = delete;
    ~EventLoopThread();

    EventLoop* start_loop(); // 启动线程，创建并返回该线程内的 EventLoop 对象指针
    EventLoop* get_loop() const { return loop.get(); }

private:
    void thread_func(); // 线程执行的函数
};
