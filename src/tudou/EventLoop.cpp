#include "EventLoop.h"
#include "Channel.h"
#include "spdlog/spdlog.h"


EventLoop::EventLoop()
    : poller(new EpollPoller()) {
}

bool EventLoop::get_is_looping() const {
    return isLooping;
}

void EventLoop::set_is_looping(bool looping) {
    isLooping = looping;
}

void EventLoop::loop(int timeoutMs) const {
    spdlog::debug("EventLoop start looping...");

    poller->set_poll_timeout_ms(timeoutMs);
    while (isLooping) {
        spdlog::debug("EventLoop is looping...");

        // 使用 poller 监听发生事件的 channels
        poller->poll();
    }

    spdlog::debug("EventLoop stop looping.");
}

void EventLoop::update_channel(Channel* channel) const {
    poller->update_channel(channel);
}

void EventLoop::remove_channel(Channel* channel) const {
    poller->remove_channel(channel);
}
