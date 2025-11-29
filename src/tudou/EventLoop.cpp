#include "EventLoop.h"

#include <assert.h>
#include <sys/eventfd.h>
#include <sys/syscall.h> // 定义 SYS_gettid
#include <unistd.h>

#include "../base/Log.h"
#include "../base/Timestamp.h"
#include "Channel.h"
#include "EpollPoller.h"

EventLoop::EventLoop() : poller(new EpollPoller()) {
}

void EventLoop::loop(int timeoutMs) {
    // LOG::LOG_DEBUG("EventLoop start looping..."); // wrk 测试时注释掉
    poller->set_poll_timeout_ms(timeoutMs);

    while (true) {
        // LOG::LOG_DEBUG("EventLoop is looping..."); // wrk 测试时注释掉

        // 使用 poller 监听发生事件的 channels
        std::vector<Channel*> activeChannels = poller->poll();
        // 通知 channel 进行事件分发处理回调
        for (auto channel : activeChannels) { channel->handle_events(Timestamp::now()); }
    }

    // LOG::LOG_DEBUG("EventLoop stop looping."); // wrk 测试时注释掉
}

void EventLoop::update_channel(Channel* channel) {
    poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) {
    poller->remove_channel(channel);
}
