/**
 * @file EpollPoller.h
 * @brief 基于 epoll 的 I/O 多路复用器，封装 epoll_create1 / epoll_ctl / epoll_wait。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * 职责：维护 epollFd 和 fd→Channel* 注册表，将内核就绪事件翻译为 Channel 列表并分发回调。
 * eventList 会根据负载因子自动扩缩。
 *
 * 线程安全：所有操作须在所属 EventLoop 线程执行。
 * 所有权：不拥有 Channel，仅保存裸指针；Channel 生存期由上层管理。
 */

#pragma once
#include <sys/epoll.h>
#include <vector>
#include <unordered_map>

class EventLoop;
class Channel;
class EpollPoller {
public:
    explicit EpollPoller(EventLoop* loop);
    ~EpollPoller();

    void poll(int timeoutMs);
    void update_channel(Channel* channel);
    void remove_channel(Channel* channel);

    bool has_channel(Channel* channel) const;

private:
    // poll() 的四步流程：wait → collect → dispatch → resize
    int get_ready_num(int timeoutMs);
    std::vector<Channel*> get_activate_channels(int numReady);
    void dispatch_events(const std::vector<Channel*>& activeChannels);
    void resize_event_list(int numReady);

private:
    static const size_t initEventListSize_ = 16;

    EventLoop* loop_;                                    // 依赖注入，所属 EventLoop 指针
    int epollFd_;                                        // epoll 文件描述符
    std::vector<epoll_event> eventList_;                 // epoll_wait 就绪事件数组（自动扩缩）
    std::unordered_map<int, Channel*> channels_;         // fd → Channel* 注册表（不拥有 Channel）
};
