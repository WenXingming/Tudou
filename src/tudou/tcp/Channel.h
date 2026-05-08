// ============================================================================
// Channel.h
// fd 事件通道，负责同步感兴趣事件并把就绪事件分发给回调。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Channel.h
// └── Channel
//     ├── Channel(loop, fd)                        # [公有] 构造即注册：把 fd 纳入所属 EventLoop/Poller
//     │   └── update_in_register()                 # [私有] 首次同步事件兴趣到 Poller
//     ├── Channel(copy)                            # [公有] 删除拷贝构造，保持 fd/loop 绑定唯一
//     ├── operator=(copy)                          # [公有] 删除拷贝赋值，禁止共享同一事件通道
//     ├── ~Channel()                               # [公有] 析构：停关注、反注册（fd 生命周期由持有者管理）
//     │   ├── disable_all()                        # [公有] 清空全部事件兴趣
//     │   │   └── update_in_register()             # [私有] 把 none-event 状态同步给 Poller
//     │   └── remove_in_register()                 # [私有] 从 Poller 中删除当前 fd（不再调用 ::close）
//     ├── handle_events()                          # [公有] Channel 唯一事件入口，先做 tie 保活再分发
//     │   └── handle_events_with_guard()           # [私有] 按“关/错/读/写”优先级回放事件
//     │       ├── handle_close_callback()          # [私有] EPOLLHUP 且无读事件时优先走关闭回调
//     │       ├── handle_error_callback()          # [私有] EPOLLERR 直接转错误回调
//     │       ├── handle_read_callback()           # [私有] EPOLLIN/EPOLLPRI 触发读回调
//     │       └── handle_write_callback()          # [私有] EPOLLOUT 触发写回调
//     ├── enable_reading()                         # [公有] 打开读事件关注
//     │   └── update_in_register()                 # [私有] 把新 events_ 同步到 Poller
//     ├── enable_writing()                         # [公有] 打开写事件关注
//     │   └── update_in_register()                 # [私有] 更新 epoll interest list
//     ├── disable_reading()                        # [公有] 关闭读事件关注
//     │   └── update_in_register()                 # [私有] 同步变更到 Poller
//     ├── disable_writing()                        # [公有] 关闭写事件关注
//     │   └── update_in_register()                 # [私有] 同步变更到 Poller
//     ├── disable_all()                            # [公有] 清空所有事件关注
//     │   └── update_in_register()                 # [私有] 让 Poller 感知当前无事件兴趣
//     ├── tie_to_object(obj)                       # [公有] 建立 weak tie，避免回调期间 owner 被析构
//     ├── set_read_callback(cb)                    # [公有] 注册读事件处理函数
//     ├── set_write_callback(cb)                   # [公有] 注册写事件处理函数
//     ├── set_close_callback(cb)                   # [公有] 注册关闭事件处理函数
//     ├── set_error_callback(cb)                   # [公有] 注册错误事件处理函数
//     ├── set_revents(revents)                     # [公有] 写入 Poller 返回的本轮就绪事件
//     ├── get_owner_loop() const                   # [公有] 返回所属 EventLoop
//     ├── get_fd() const                           # [公有] 返回绑定 fd
//     ├── is_none_event() const                    # [公有] 判断当前是否无事件关注
//     ├── is_writing() const                       # [公有] 判断是否关注写事件
//     ├── is_reading() const                       # [公有] 判断是否关注读事件
//     └── get_events() const                       # [公有] 返回当前事件兴趣掩码
// ============================================================================

#pragma once

#include <functional>
#include <memory>

class EventLoop;

// Channel 只管理单个 fd 的事件兴趣与回调分发，不参与更高层业务编排。
class Channel {
public:
    using EventCallback = std::function<void(Channel&)>;

    explicit Channel(EventLoop* loop, int fd);
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel();

    void set_read_callback(EventCallback cb);
    void set_write_callback(EventCallback cb);
    void set_close_callback(EventCallback cb);
    void set_error_callback(EventCallback cb);
    void set_revents(uint32_t revents);
    void handle_events(); // Channel 的统一事件入口。
    void tie_to_object(const std::shared_ptr<void>& obj); // 回调期间保活 owner。

    EventLoop* get_owner_loop() const;
    int get_fd() const;
    void enable_reading();
    void enable_writing();
    void disable_reading();
    void disable_writing();
    void disable_all();
    bool is_none_event() const;
    bool is_writing() const;
    bool is_reading() const;
    uint32_t get_events() const;

private:
    void update_in_register(); // 把当前 events_ 同步到 Poller。
    void remove_in_register();
    void handle_events_with_guard(); // 按关闭/错误/读/写顺序分发事件。

    void handle_read_callback();
    void handle_write_callback();
    void handle_close_callback();
    void handle_error_callback();

private:
    static const uint32_t kNoneEvent_;
    static const uint32_t kReadEvent_;
    static const uint32_t kWriteEvent_;

    EventLoop* loop_; // 所属 EventLoop，非 owning。

    int fd_; // 当前 Channel 绑定的 fd。
    uint32_t events_; // 当前感兴趣事件掩码。
    uint32_t revents_; // Poller 返回的本轮就绪事件掩码。

    std::weak_ptr<void> tie_; // 用于在回调期间暂时保活所有者对象。
    bool isTied_; // 是否已启用 tie 机制。

    EventCallback readCallback_; // 读事件回调。
    EventCallback writeCallback_; // 写事件回调。
    EventCallback closeCallback_; // 关闭事件回调。
    EventCallback errorCallback_; // 错误事件回调。
};
