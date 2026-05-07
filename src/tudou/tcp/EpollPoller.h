// ============================================================================
// EpollPoller.h
// epoll 封装层，负责注册 Channel、等待就绪事件并回放到 Channel。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// EpollPoller.h
// └── EpollPoller
//     ├── EpollPoller(loop)                       # [公有] 构造 epoll fd 并初始化事件缓冲区
//     ├── ~EpollPoller()                          # [公有] 析构：关闭 epoll fd
//     ├── poll(timeoutMs)                         # [公有] epoll 主干：等待、翻译、分发并调节容量
//     │   ├── get_ready_num(timeoutMs)            # [私有] 调用 epoll_wait 拿到本轮就绪数
//     │   ├── get_activate_channels(numReady)     # [私有] 把内核事件翻译成 Channel 列表
//     │   ├── dispatch_events(activeChannels)     # [私有] 顺序调用 Channel::handle_events()
//     │   └── resize_event_list(numReady)         # [私有] 按负载伸缩 epoll 结果缓冲区
//     ├── update_channel(channel)                 # [公有] ADD/MOD 一个 Channel 到 epoll 注册表
//     ├── remove_channel(channel)                 # [公有] DEL 一个 Channel 并同步移出 channels_
//     └── has_channel(channel) const              # [公有] 查询 fd 是否已被当前 Poller 持有
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

    void poll(int timeoutMs); // epoll 主入口：等待、翻译并分发就绪事件。
    void update_channel(Channel* channel); // 把 Channel 的当前兴趣集同步到 epoll。
    void remove_channel(Channel* channel);
    bool has_channel(Channel* channel) const;

private:
    int get_ready_num(int timeoutMs);
    std::vector<Channel*> get_activate_channels(int numReady); // 回填 revents_ 并收集活跃 Channel。
    void dispatch_events(const std::vector<Channel*>& activeChannels);
    void resize_event_list(int numReady); // 按负载调节 eventList_ 容量。

private:
    static const size_t initEventListSize_ = 16;

    EventLoop* loop_; // 所属 EventLoop，限定线程边界。
    int epollFd_; // epoll 文件描述符。
    std::vector<epoll_event> eventList_; // epoll_wait 使用的结果缓冲区。
    std::unordered_map<int, Channel*> channels_; // fd 到 Channel 的注册表，不拥有 Channel。
};
