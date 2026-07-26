// ============================================================================
// EpollPoller 在 EventLoop 中封装 epoll，负责等待 I/O 就绪事件。
// 它维护 Channel 的非 owning 注册表，将 epoll 结果整理成活跃 Channel 列表。
// ============================================================================

#pragma once
#include "base/ScopedFd.h"

#include <sys/epoll.h>
#include <vector>
#include <unordered_map>

class EventLoop;
class Channel;

class EpollPoller {
public:
    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller();

    const std::vector<Channel*>& poll(int timeoutMs);

    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

    bool has_channel(Channel* channel) const;

private:
    int collect_ready_num(int timeoutMs);
    void collect_active_channels(int numReady);
    void resize_event_list(int numReady);

private:
    EventLoop* loop_;                                       // 所属 EventLoop，限定线程边界。

    ScopedFd epollFd_;                                      // epoll 文件描述符。
    std::unordered_map<int, Channel*> channels_;            // fd 到 Channel 的注册表，不拥有 Channel。

    static constexpr size_t kInitEventListSize_ = 16;       // 初始事件列表容量。static 节省空间，避免每个实例多占 8 字节
    std::vector<epoll_event> eventList_;                    // epoll_wait 使用的结果缓冲区，后续按需扩容和缩容。
    std::vector<Channel*> activeChannels_;                  // 每轮 poll 的就绪 Channel 列表，复用避免反复堆分配。
};
