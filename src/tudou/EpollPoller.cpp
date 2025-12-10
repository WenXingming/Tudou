/**
 * @file EpollPoller.h
 * @brief 基于 epoll 的 Poller 实现 — 多路 I/O 事件分发器（Reactor 的 I/O 多路复用层）
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "EpollPoller.h"

#include <cassert>
#include <cstring>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "Channel.h"

EpollPoller::EpollPoller() {
    epollFd = ::epoll_create1(EPOLL_CLOEXEC); // EPOLL_CLOEXEC 避免 fork 出来的子进程继承 epoll fd
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

// 使用 epoll_wait() 返回活动的 channels 列表
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

// 根据 epoll_wait 返回的就绪事件（存放在 eventList 中），找到对应的 channel 并设置 revent、维护 channels
std::vector<Channel*> EpollPoller::get_activate_channels(int numReady) {
    std::vector<Channel*> activeChannels;

    for (int i = 0; i < numReady; ++i) {
        const epoll_event& event = this->eventList[i];
        int fd = event.data.fd;
        uint32_t revent = event.events;

        auto it = channels.find(fd);
        assert(it != channels.end()); // 否则 Error：说明 epoll 和 channels 不同步
        Channel* channel = it->second;
        channel->set_revents(revent);
        activeChannels.push_back(channel);
    }

    return std::move(activeChannels);
}

// 对每一个 active channel，进行事件分发, 通知 channel 进行事件分发处理回调
void EpollPoller::dispatch_events(const std::vector<Channel*>& activeChannels) {
    for (auto channel : activeChannels) {
        channel->handle_events();
    }
}

// eventList 自动扩容和缩减
void EpollPoller::resize_event_list(const int numReady) {
    if (numReady == eventList.size()) {
        eventList.resize(static_cast<size_t>(eventList.size() * 1.5));
    }
    else if (eventList.size() > eventListSize && numReady < eventList.size() * 0.25) {
        eventList.resize(static_cast<size_t>(eventList.size() * 0.5));
    }
    else;
}

// 维护注册中心 epollfd、channels。使用 epoll_ctl() 更新, 包括 EPOLL_CTL_ADD、EPOLL_CTL_MOD
void EpollPoller::update_channel(Channel* channel) {
    int fd = channel->get_fd();
    uint32_t event = channel->get_events();

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.fd = fd;
    ev.events = event;

    spdlog::info("EpollPoller::update_channel(): fd is {}", fd);

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

// 维护注册中心 epollfd、channels。使用 epoll_ctl() 删除, 包括 EPOLL_CTL_DEL
void EpollPoller::remove_channel(Channel* channel) {
    int fd = channel->get_fd();

    // epollfd、channels 应该同步。后续还有 channels 和 Acceptor/TcpConnection 之间的同步问题需要解决（如何保证同步？）
    int epollCtlRet = epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);
    assert(epollCtlRet == 0);
    channels.erase(fd);
}
