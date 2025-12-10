/**
 * @file EpollPoller.h
 * @brief 多路 I/O 事件分发器（Reactor 的 I/O 多路复用层）。基于 epoll 的 Poller 实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "EpollPoller.h"
#include "EventLoop.h"
#include "Channel.h"
#include "spdlog/spdlog.h"

#include <unistd.h>
#include <cassert>
#include <cstring>


EpollPoller::EpollPoller(EventLoop* _loop)
    : loop(_loop)
    , epollFd(::epoll_create1(EPOLL_CLOEXEC)) /* EPOLL_CLOEXEC 避免 fork 出来的子进程继承 epoll fd */
    , eventList(initEventListSize)
    , pollTimeoutMs(initPollTimeoutMs)
    , channels() {

    assert(epollFd > 0);
}

EpollPoller::~EpollPoller() {
    int ret = ::close(epollFd);
    assert(ret == 0);
}

void EpollPoller::set_poll_timeout_ms(int timeoutMs) {
    pollTimeoutMs = timeoutMs;
}

int EpollPoller::get_poll_timeout_ms() const {
    return pollTimeoutMs;
}

void EpollPoller::poll() {
    spdlog::info("Epoll is running... poller monitors channels's size is: {}", channels.size());

    int numReady = get_ready_num();
    std::vector<Channel*> activeChannels = get_activate_channels(numReady);
    dispatch_events(activeChannels);
    resize_event_list(numReady); // get_activate_channels 完成后调用，防止使用过程中 eventList 大小变化

}

bool EpollPoller::has_channel(Channel* channel) const {
    int fd = channel->get_fd();
    auto it = channels.find(fd);
    return it != channels.end() && it->second == channel;
}

void EpollPoller::update_channel(Channel* channel) {
    loop->assert_in_loop_thread();

    int fd = channel->get_fd();
    uint32_t events = channel->get_events();

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    // ev.data.ptr = static_cast<void*>(channel); // 不知道为什么一用 data.ptr 就会出错

    auto findIt = channels.find(fd);
    if (findIt == channels.end()) {
        int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_ADD, ev.data.fd, &ev);
        channels[fd] = channel;
        assert(epollCtlRet == 0);
    }
    else {
        int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_MOD, ev.data.fd, &ev);
        assert(epollCtlRet == 0);
        assert(channels[fd] == channel);
    }
}

void EpollPoller::remove_channel(Channel* channel) {
    int fd = channel->get_fd();

    // epollfd、channels 应该同步
    int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
    assert(epollCtlRet == 0);
    channels.erase(fd);
}

int EpollPoller::get_ready_num() {
    int numReady = epoll_wait(epollFd
        , eventList.data()
        , static_cast<int>(eventList.size())
        , pollTimeoutMs
    );
    if (numReady < 0) {
        spdlog::error("EpollPoller::poll() error: epoll_wait return {}", numReady);
    }
    spdlog::info("EpollPoller::poll() return numReady: {}", numReady);

    return numReady;
}

std::vector<Channel*> EpollPoller::get_activate_channels(int numReady) {
    std::vector<Channel*> activeChannels;

    for (int i = 0; i < numReady; ++i) {
        const epoll_event& event = this->eventList[i];
        int fd = event.data.fd;
        uint32_t revent = event.events;
        // Channel* channel = event.data.ptr ? static_cast<Channel*>(event.data.ptr) : nullptr;

        auto it = channels.find(fd);
        assert(it != channels.end()); // 否则说明 epoll 和 channels 不同步
        Channel* channel = it->second;
        channel->set_revents(revent);
        activeChannels.push_back(channel);
    }

    return std::move(activeChannels);
}

void EpollPoller::dispatch_events(const std::vector<Channel*>& activeChannels) {
    for (auto channel : activeChannels) {
        channel->handle_events();
    }
}

void EpollPoller::resize_event_list(const int numReady) {
    double loadFactor = static_cast<double>(numReady) / eventList.size();
    double expandThreshold = 0.9;
    double shrinkThreshold = 0.25;
    double expandRatio = 1.5;
    double shrinkRatio = 0.5;

    if (loadFactor >= expandThreshold) {
        eventList.resize(static_cast<size_t>(eventList.size() * expandRatio));
    }
    else if (eventList.size() > initEventListSize && loadFactor <= shrinkThreshold) {
        eventList.resize(static_cast<size_t>(eventList.size() * shrinkRatio));
    }
    else;
}