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
 * - EventLoop 通常与线程一一绑定（One loop per thread），非线程安全方法（例如 IO 线程的 TcpConnection 的初始化等）须在所属线程调用。
 */

#pragma once

#include <memory>
#include <queue>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>

#include "EpollPoller.h"

class Channel;
class EventLoop {
    const static int pollTimeoutMs;                         // epoll_wait 超时时间，单位毫秒

private:
    std::unique_ptr<EpollPoller> poller;                    // 拥有 poller，控制其生命期。智能指针，自动析构
    std::atomic<bool> isLooping;                            // 标记事件循环状态
    std::atomic<bool> isQuit;                               // 标记退出循环

    const std::thread::id threadId;                         // 记录所属线程 id，断言线程安全使用

    int wakeupFd;                                           // 用于唤醒 loop 线程的 fd（wait/notify 机制）
    std::unique_ptr<Channel> wakeupChannel;                 // 用于唤醒 loop 线程的 Channel

    std::atomic<bool> isCallingPendingFunctors;             // 标记当前 loop 线程是否正在执行 pending functors
    std::queue<std::function<void()>> pendingFunctors;      // 存放 loop 线程需要执行的函数列表
    std::mutex mtx;                                         // 保护 pendingFunctors 的互斥锁（多线程只要有写入操作就需要加锁）

public:
    EventLoop();
    ~EventLoop();
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;


    void loop(int timeoutMs = 10000);
    void quit();                                            // 退出事件循环

    bool has_channel(Channel* channel) const;
    void update_channel(Channel* channel) const;
    void remove_channel(Channel* channel) const;

    bool is_in_loop_thread() const;                         // 判断是否在当前线程调用
    void assert_in_loop_thread() const;                     // 确保在当前线程调用

    void run_in_loop(const std::function<void()>& cb);      // 如果在 loop 线程，直接执行 cb，否则入队
    void queue_in_loop(const std::function<void()>& cb);    // 将函数入队到 pendingFunctors 中，唤醒 loop 线程在相应线程执行 cb
    void wakeup();                                          // 唤醒 loop 所在线程，从 poll 阻塞中被唤醒。因为如果当前没有事件就会阻塞，此时增加一个可读事件，使 poll 被唤醒处理完该简单事件迅速返回，然后执行 pending functors。所以 wakeup 的主要功能是在没有事件时打断 poll 阻塞，快速返回处理 pending functors

private:
    void on_read();                                         // wakeupFd 所属的 wakeupChannel 的读事件回调
    void do_pending_functors();                             // 执行 pendingFunctors 中的函数
};
