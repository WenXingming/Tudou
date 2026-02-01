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

EpollPoller::EpollPoller(EventLoop* loop)
    : loop_(loop)
    , epollFd_(::epoll_create1(EPOLL_CLOEXEC)) // EPOLL_CLOEXEC 避免 fork 出来的子进程继承 epoll fd
    , eventList_(EpollPoller::initEventListSize_)
    , channels_() {

    if (epollFd_ < 0) {
        spdlog::critical("EpollPoller::EpollPoller() error: epoll_create1 failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }
}

EpollPoller::~EpollPoller() {
    int ret = ::close(epollFd_);
    assert(ret == 0);
}

void EpollPoller::poll(int timeoutMs) {
    spdlog::debug("Epoll is running... poller monitors channels's size is: {}", channels_.size());

    int numReady = get_ready_num(timeoutMs);
    std::vector<Channel*> activeChannels = get_activate_channels(numReady);
    dispatch_events(activeChannels);
    resize_event_list(numReady); // get_activate_channels 完成后调用，防止使用过程中 eventList_ 大小变化
}

void EpollPoller::update_channel(Channel* channel) {
    loop_->assert_in_loop_thread();

    int fd = channel->get_fd();
    uint32_t events = channel->get_events();

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events; // events | EPOLLET 边沿触发
    ev.data.fd = fd;
    // ev.data.ptr = static_cast<void*>(channel); // 不知道为什么一用 data.ptr 就会出错

    auto findIt = channels_.find(fd);
    if (findIt == channels_.end()) {
        channels_[fd] = channel;
        int epollCtlRet = epoll_ctl(epollFd_, EPOLL_CTL_ADD, ev.data.fd, &ev);
        if (epollCtlRet != 0) {
            spdlog::error("epoll_ctl ADD failed, fd={}, events={}, errno={} ({})", fd, events, errno, strerror(errno));
            assert(false);
        }
    }
    else {
        assert(channels_[fd] == channel);
        int epollCtlRet = epoll_ctl(epollFd_, EPOLL_CTL_MOD, ev.data.fd, &ev);
        if (epollCtlRet != 0) {
            spdlog::error("epoll_ctl MOD failed, fd={}, events={}, errno={} ({})", fd, events, errno, strerror(errno));
            assert(false);
        }
    }
}

void EpollPoller::remove_channel(Channel* channel) {
    loop_->assert_in_loop_thread();

    // epollfd、channels 应该同步
    int fd = channel->get_fd();
    channels_.erase(fd);
    int epollCtlRet = epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    if (epollCtlRet != 0) {
        spdlog::error("epoll_ctl DEL failed, fd={}, errno={} ({})", fd, errno, strerror(errno));
        assert(false);
    }
}

bool EpollPoller::has_channel(Channel* channel) const {
    loop_->assert_in_loop_thread();

    int fd = channel->get_fd();
    auto findIt = channels_.find(fd);
    if (findIt == channels_.end()) {
        return false;
    }
    assert(findIt->second == channel);
    return true;
}

int EpollPoller::get_ready_num(int timeoutMs) {
    int numReady = epoll_wait(epollFd_
        , eventList_.data()
        , static_cast<int>(eventList_.size())
        , timeoutMs
    );
    if (numReady < 0) {
        spdlog::error("EpollPoller::poll() error: epoll_wait return {}", numReady);
    }
    spdlog::debug("EpollPoller::poll() return numReady: {}", numReady);

    return numReady;
}

std::vector<Channel*> EpollPoller::get_activate_channels(int numReady) {
    std::vector<Channel*> activeChannels;

    for (int i = 0; i < numReady; ++i) {
        const epoll_event& event = this->eventList_[i];
        int fd = event.data.fd;
        uint32_t revent = event.events;
        // Channel* channel = event.data.ptr ? static_cast<Channel*>(event.data.ptr) : nullptr;

        auto findIt = channels_.find(fd);
        assert(findIt != channels_.end()); // 否则说明 epoll 和 channels 不同步
        Channel* channel = findIt->second;
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
    double loadFactor = static_cast<double>(numReady) / eventList_.size();
    double expandThreshold = 0.9;
    double shrinkThreshold = 0.25;
    double expandRatio = 1.5;
    double shrinkRatio = 0.5;

    if (loadFactor >= expandThreshold) {
        eventList_.resize(static_cast<size_t>(eventList_.size() * expandRatio));
        return;
    }
    if (eventList_.size() > EpollPoller::initEventListSize_ && loadFactor <= shrinkThreshold) {
        eventList_.resize(static_cast<size_t>(eventList_.size() * shrinkRatio));
        return;
    }
}