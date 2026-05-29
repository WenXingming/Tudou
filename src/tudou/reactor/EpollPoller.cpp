// ============================================================================
// EpollPoller.cpp
// epoll 封装实现，保持“等待 -> 翻译 -> 分发 -> 调整容量”的单层流程。
// ============================================================================

#include "tudou/reactor/EpollPoller.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/reactor/Channel.h"
#include "spdlog/spdlog.h"
#include <cassert>
#include <cstring>

EpollPoller::EpollPoller(EventLoop* loop)
    : loop_(loop)
    , epollFd_(::epoll_create1(EPOLL_CLOEXEC)) // EPOLL_CLOEXEC 确保 exec 后 fd 被自动关闭，避免 fork 的子进程误继承父进程的 epollFd 导致资源泄漏或竞争
    , channels_()
    , initEventListSize_(16)
    , eventList_(initEventListSize_) {
    if (epollFd_.fd() < 0) {
        spdlog::critical("EpollPoller: epoll_create1 failed, errno={} ({})", errno, strerror(errno));
        assert(false);
    }
}

EpollPoller::~EpollPoller() = default;

const std::vector<Channel*>& EpollPoller::poll(int timeoutMs) {
    const int numReady = get_ready_num(timeoutMs);
    collect_active_channels(numReady);
    resize_event_list(numReady);
    return activeChannels_;
}

void EpollPoller::update_channel(Channel* channel) {
    assert(loop_->is_in_loop_thread());

    const int fd = channel->get_fd();
    const uint32_t events = channel->get_events();

    struct epoll_event ev;
    std::memset(&ev, 0, sizeof(ev));
    ev.events = events;
    ev.data.ptr = channel; // 注意：epoll_event.data 是 union，data.fd 和 data.ptr 不能同时使用

    const auto findIt = channels_.find(fd);
    const int operation = (findIt == channels_.end()) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;
    if (operation == EPOLL_CTL_ADD) {
        channels_[fd] = channel;
    }
    else {
        assert(findIt->second == channel);
    }

    int epollCtlRet = epoll_ctl(epollFd_.fd(), operation, fd, &ev);
    if (epollCtlRet != 0) {
        spdlog::error("epoll_ctl {} failed, fd={}, events={}, errno={} ({})",
            operation == EPOLL_CTL_ADD ? "ADD" : "MOD",
            fd,
            events,
            errno,
            strerror(errno));
        assert(false);
    }
}

void EpollPoller::remove_channel(Channel* channel) {
    assert(loop_->is_in_loop_thread());

    // epollfd、channels 应该同步
    int fd = channel->get_fd();
    channels_.erase(fd);
    int epollCtlRet = epoll_ctl(epollFd_.fd(), EPOLL_CTL_DEL, fd, nullptr);
    if (epollCtlRet != 0) {
        spdlog::error("epoll_ctl DEL failed, fd={}, errno={} ({})", fd, errno, strerror(errno));
        assert(false);
    }
}

bool EpollPoller::has_channel(Channel* channel) const {
    assert(loop_->is_in_loop_thread());

    const int fd = channel->get_fd();
    const auto findIt = channels_.find(fd);
    if (findIt == channels_.end()) {
        return false;
    }
    assert(findIt->second == channel);
    return true;
}

int EpollPoller::get_ready_num(int timeoutMs) {
    int numReady = ::epoll_wait(epollFd_.fd(), eventList_.data(),
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

void EpollPoller::collect_active_channels(int numReady) {
    activeChannels_.clear();
    activeChannels_.reserve(numReady);

    for (int i = 0; i < numReady; ++i) {
        auto* channel = static_cast<Channel*>(eventList_[i].data.ptr);
        assert(channel != nullptr);
        channel->set_revents(eventList_[i].events);
        activeChannels_.push_back(channel);
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