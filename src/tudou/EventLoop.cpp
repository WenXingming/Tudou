#include "EventLoop.h"

#include <assert.h>
#include <sys/eventfd.h>
#include <sys/syscall.h> // 定义 SYS_gettid
#include <unistd.h>

#include "../base/Log.h"
#include "../base/Timestamp.h"
#include "Channel.h"
#include "EpollPoller.h"

EventLoop::EventLoop()
    : poller(new EpollPoller()) {
}

void EventLoop::loop(int timeoutMs) {
    // wrk 测试时注释掉 LOG
    LOG::LOG_INFO("EventLoop start looping...");
    poller->set_poll_timeout_ms(timeoutMs);
    while (true) {
        // wrk 测试时注释掉 LOG
        LOG::LOG_INFO("EventLoop is looping...");

        // 使用 poller 监听发生事件的 channels
        std::vector<Channel*> activeChannels = poller->poll();
        // 通知 channel 进行事件分发处理回调
        for (auto channel : activeChannels) {
            channel->handle_events(Timestamp::now());
        }
    }

    // wrk 测试时注释掉 LOG
    LOG::LOG_INFO("EventLoop stop looping.");
}

void EventLoop::update_channel(Channel* channel) {
    poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
    poller->remove_channel(channel);
}
