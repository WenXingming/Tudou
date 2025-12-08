/**
 * @file EventLoop.h
 * @brief 事件循环（Reactor）核心类，驱动 I/O 事件的收集与回调执行。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * 说明：
 * - 封装一个 Poller 如 EpollPoller（持有 poller 的唯一所有权（std::unique_ptr），在 EventLoop 析构时自动释放）
 * -
 * - 提供事件循环 loop() 方法，持续监听和分发事件：
 * -    1. 持续调用 poller->poll(timeoutMs) 获取 Active Channels
 * -    2. 对获取到的 Active Channels，调用其 handle_events() 方法，触发相应的事件回调
 *
 * - 对外暴露 update_channel()/remove_channel() 方法，用于 Channel 向 EventLoop 注册或取消自身。
 *
 * 线程模型与约定：
 * - EventLoop ，禁止拷贝与赋值以避免多个所有者。
 * - EventLoop 通常与线程一一绑定（一个线程一个 EventLoop），非线程安全方法（例如 IO 线程的 TcpConnection 的初始化等）须在所属线程调用。
 */

#pragma once
#include <memory>
#include <queue>
#include <functional>
#include <mutex>
#include <cassert>

#include "EpollPoller.h"

class Channel;
class EventLoop {
private:
    std::unique_ptr<EpollPoller> poller;                    // 拥有 poller，控制其生命期。智能指针，自动析构
    bool isLooping;                                         // 标记事件循环状态
    bool isQuit;                                            // 标记退出循环

    std::mutex mtx;                                         // 保护函数队列的互斥锁（其他线程入队时需要加锁）
    std::queue<std::function<void()>> pendingFunctors;      // 存放 loop 线程需要执行的函数列表

public:
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void quit();                                            // 退出事件循环

    void loop(int timeoutMs = 10000);
    void update_channel(Channel* channel) const;
    void remove_channel(Channel* channel) const;

    void run_in_loop(const std::function<void()>& cb);      // 如果在 loop 线程，直接执行 cb，否则入队（后续加入唤醒机制）

private:
    void assert_in_loop_thread() const;                     // 确保在当前线程调用
    bool is_in_loop_thread() const;                         // 判断是否在当前线程调用
    void queue_in_loop(const std::function<void()>& cb);    // 将函数入队到 pendingFunctors 中
    void do_pending_functors();                             // 执行 pendingFunctors 中的函数
};
