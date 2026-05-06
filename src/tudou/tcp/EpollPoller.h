// ============================================================================
// EpollPoller.h
// epoll 封装层，负责注册 Channel、等待就绪事件并回放到 Channel。
// ============================================================================

#pragma once
#include <sys/epoll.h>
#include <vector>
#include <unordered_map>

class EventLoop;
class Channel;

// EpollPoller 是 EventLoop 的底层 I/O 复用器，只管理 epoll 相关细节。
class EpollPoller {
public:
    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller();

    /**
     * @brief 执行一次 epoll_wait 并分发本轮就绪事件。
     * @param timeoutMs epoll_wait 超时时间，单位毫秒。
     */
    void poll(int timeoutMs);

    /**
     * @brief 注册或更新一个 Channel。
     * @param channel 需要同步到 epoll 的 Channel。
     */
    void update_channel(Channel* channel);

    /**
     * @brief 从 epoll 中移除一个 Channel。
     * @param channel 需要移除的 Channel。
     */
    void remove_channel(Channel* channel);

    /**
     * @brief 判断当前 Poller 是否已经持有该 Channel。
     * @param channel 需要检查的 Channel。
     * @return true 表示当前已注册该 Channel。
     */
    bool has_channel(Channel* channel) const;

private:
    /**
     * @brief 执行一次 epoll_wait。
     * @param timeoutMs epoll_wait 超时时间，单位毫秒。
     * @return 当前批次就绪事件数。
     */
    int get_ready_num(int timeoutMs);

    /**
     * @brief 把内核就绪事件翻译成 Channel 列表。
     * @param numReady 当前批次就绪事件数。
     * @return 当前批次活跃 Channel 列表。
     */
    std::vector<Channel*> get_activate_channels(int numReady);

    /**
     * @brief 顺序回放本轮活跃 Channel 的事件。
     * @param activeChannels 当前批次活跃 Channel 列表。
     */
    void dispatch_events(const std::vector<Channel*>& activeChannels);

    /**
     * @brief 按当前负载调整 epoll_wait 结果缓冲区容量。
     * @param numReady 当前批次就绪事件数。
     */
    void resize_event_list(int numReady);

private:
    static const size_t initEventListSize_ = 16;

    EventLoop* loop_; // 所属 EventLoop，限定线程边界。
    int epollFd_; // epoll 文件描述符。
    std::vector<epoll_event> eventList_; // epoll_wait 使用的结果缓冲区。
    std::unordered_map<int, Channel*> channels_; // fd 到 Channel 的注册表，不拥有 Channel。
};
