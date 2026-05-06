// ============================================================================
// EpollPoller.cpp
// epoll 封装实现，保持“等待 -> 翻译 -> 分发 -> 调整容量”的单层流程。
// ============================================================================

#include "EpollPoller.h"
#include "EventLoop.h"
#include "Channel.h"
#include "spdlog/spdlog.h"
#include <unistd.h>
#include <cassert>
#include <cstring>

EpollPoller::EpollPoller(EventLoop* loop)
    : loop_(loop)
    , epollFd_(::epoll_create1(EPOLL_CLOEXEC)) // EPOLL_CLOEXEC 确保 exec 后 fd 被自动关闭，避免 fork 的子进程误继承父进程的 epollFd 导致资源泄漏或竞争
    , eventList_(initEventListSize_)
    , channels_() {
    if (epollFd_ < 0) {
        spdlog::critical("EpollPoller: epoll_create1 failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }
}

EpollPoller::~EpollPoller() {
    int ret = ::close(epollFd_);
    assert(ret == 0);
}

void EpollPoller::poll(int timeoutMs) {
    const int numReady = get_ready_num(timeoutMs);
    const auto activeChannels = get_activate_channels(numReady);
    dispatch_events(activeChannels);
    resize_event_list(numReady);
}

void EpollPoller::update_channel(Channel* channel) {
    loop_->assert_in_loop_thread();

    int fd = channel->get_fd();
    uint32_t events = channel->get_events();

    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.fd = fd;
    // 注意：epoll_event.data 是 union，data.fd 和 data.ptr 不能同时使用。
    // 当前使用 data.fd + channels_ hash map 查找 Channel*；若改用 data.ptr
    // 可省去一次哈希查找，但需要去掉 data.fd 的赋值。保持现状以简化调试。

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
    int numReady = ::epoll_wait(epollFd_, eventList_.data(),
        static_cast<int>(eventList_.size()), timeoutMs);
    if (numReady < 0) {
        // 非致命；其他错误记录后同样安全返回 0
        if (errno != EINTR) {
            spdlog::error("EpollPoller::get_ready_num(): epoll_wait error, errno={} ({})", errno, strerror(errno));
        }
        numReady = 0;
    }
    spdlog::debug("EpollPoller::get_ready_num(): numReady={}", numReady);
    return numReady;
}

std::vector<Channel*> EpollPoller::get_activate_channels(int numReady) {
    std::vector<Channel*> activeChannels;
    activeChannels.reserve(numReady);

    for (int i = 0; i < numReady; ++i) {
        int fd = eventList_[i].data.fd;
        auto it = channels_.find(fd);
        assert(it != channels_.end()); // epoll 与 channels_ 必须同步
        Channel* channel = it->second;
        channel->set_revents(eventList_[i].events);
        activeChannels.push_back(channel);
    }
    return activeChannels; // NRVO，勿加 std::move
}

void EpollPoller::dispatch_events(const std::vector<Channel*>& activeChannels) {
    for (Channel* channel : activeChannels) {
        channel->handle_events();
    }
}

void EpollPoller::resize_event_list(int numReady) {
    // 根据负载因子自动扩缩 eventList，避免就绪事件被截断或内存浪费
    constexpr double expandThreshold = 0.9;
    constexpr double shrinkThreshold = 0.25;
    constexpr double expandRatio = 1.5;
    constexpr double shrinkRatio = 0.5;

    double loadFactor = static_cast<double>(numReady) / eventList_.size();
    if (loadFactor >= expandThreshold) {
        eventList_.resize(static_cast<size_t>(eventList_.size() * expandRatio));
    }
    else if (eventList_.size() > initEventListSize_ && loadFactor <= shrinkThreshold) {
        eventList_.resize(static_cast<size_t>(eventList_.size() * shrinkRatio));
    }
}