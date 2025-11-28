/**
 * @file EpollPoller.h
 * @brief 基于 epoll 的 Poller 实现 — 多路 I/O 事件分发器（Reactor 的 I/O 多路复用层）
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "EpollPoller.h"

#include <assert.h>
#include <string.h>
#include <unistd.h>

#include "../base/Log.h"
#include "Channel.h"

EpollPoller::EpollPoller() : epollFd(::epoll_create1(EPOLL_CLOEXEC)) {
    assert(epollFd > 0);
}

EpollPoller::~EpollPoller() {
    assert(epollFd > 0);
    ::close(epollFd);
}

void EpollPoller::set_poll_timeout_ms(int timeoutMs) {
    pollTimeoutMs = timeoutMs;
}

int EpollPoller::get_poll_timeout_ms() const {
    return pollTimeoutMs;
}

/// @brief 使用 epoll_wait() 返回活动的 channels 列表
/// @param timeoutMs
/// @return
std::vector<Channel*> EpollPoller::poll() {
    LOG::LOG_DEBUG("Epoll is running... poller monitors channels's size is: %d", channels.size());

    std::vector<Channel*> activeChannels;

    int numReady = epoll_wait(epollFd, eventList.data(), static_cast<int>(eventList.size()), pollTimeoutMs);
    if (numReady > 0) {
        LOG::LOG_DEBUG("Epoll is running... activeChannels's size is: %d", numReady);
        activeChannels.reserve(numReady);
        activeChannels = get_activate_channels(numReady);
        resize_event_list(numReady);
    }

    return std::move(activeChannels);
}

/// @brief 根据 epoll_wait 返回的就绪事件（存放在 eventList 中），找到对应的 channel 并设置 revent、维护 channels
/// @param numReady
/// @return 返回活动的 channels 列表：activeChannels
std::vector<Channel*> EpollPoller::get_activate_channels(int numReady) const {
    std::vector<Channel*> activeChannels;

    for (int i = 0; i < numReady; ++i) {
        const epoll_event& event = this->eventList[i];
        int fd = event.data.fd;
        auto revent = event.events;

        auto it = channels.find(fd);
        assert(it != channels.end()); // 否则 Error：说明 epoll 和 channels 不同步
        Channel* channel = it->second;
        channel->set_revent(revent);
        activeChannels.push_back(channel);
    }

    return std::move(activeChannels);
}

/// @brief eventList 自动扩容和缩减
/// @param numReady
void EpollPoller::resize_event_list(int numReady) {
    if (numReady == eventList.size()) {
        eventList.resize(eventList.size() * 1.5);
    }
    else if (numReady < eventList.size() * 0.25 && eventList.size() > eventListSize) {
        eventList.resize(eventList.size() * 0.5);
    }
}

/// @brief 维护注册中心 epollfd、channels。使用 epoll_ctl(), 包括 add、del、mod
void EpollPoller::update_channel(Channel* channel) {
    int fd = channel->get_fd();
    auto event = channel->get_event();

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    ev.events = event;

    // LOG::LOG_DEBUG("update_channel(): fd is %d", fd);
    auto findIt = channels.find(fd);
    if (findIt == channels.end()) {
        int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_ADD, ev.data.fd, &ev);
        channels[fd] = channel;
        assert(epollCtlRet == 0);
    }
    else {
        int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_MOD, ev.data.fd, &ev);
        assert(channels[fd] == channel);
        channels[fd] = channel; // 思考是否需要？按理说二者相等，上面也进行了断言。实际上不需要，其他地方更改 channel
        // 都是通过指针已经更改了原对象
        assert(epollCtlRet == 0);
    }
}

void EpollPoller::remove_channel(Channel* channel) {
    int fd = channel->get_fd();

    // epollfd、channels should be synchronous.
    int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
    channels.erase(fd);
    assert(epollCtlRet == 0);
}
