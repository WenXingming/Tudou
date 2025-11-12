/**
 * @file EventLoop.h
 * @brief 事件循环（Reactor）核心类，驱动 I/O 事件的收集与回调执行。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "EventLoop.h"
#include <iostream>
#include <assert.h>
#include <thread>
#include <unistd.h>      // 定义 syscall, pid_t
#include <sys/eventfd.h>
#include <sys/syscall.h> // 定义 SYS_gettid

#include "../base/Timestamp.h"
#include "../base/Log.h"
#include "Poller.h"
#include "Channel.h"

EventLoop::EventLoop()
    : poller(Poller::new_default_poller(this))
    , pollTimeoutMs(5000) {}

EventLoop::~EventLoop() {}

// 事件循环。Rector 模式：通过 poller（IO多路复用）获取活动的 channels，然后调用 channel 的 publish_events 进行事件分发回调
void EventLoop::loop() {
    LOG::LOG_DEBUG("EventLoop start looping...");
    while (true) {
        LOG::LOG_DEBUG("EventLoop is looping...");
        // 使用 poller 监听发生事件的 channels
        std::vector<Channel*> activeChannels = poller->poll(pollTimeoutMs);
        // 通知 channel 处理回调
        for (auto channel : activeChannels) {
            channel->publish_events(Timestamp::now());
        }
    }
    LOG::LOG_DEBUG("EventLoop stop looping.");
}

void EventLoop::update_channel(Channel* channel) {
    poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
    poller->remove_channel(channel);
}
