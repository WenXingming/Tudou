// ============================================================================
// Channel 在构造时注册到 Poller、析构时从 Poller 注销
// Channel 负责同步事件兴趣并把 Poller 返回的就绪事件分发给回调。
// ============================================================================

#pragma once

#include <functional>
#include <memory>
#include <cstdint>

class EventLoop;

class Channel {
public:
    using EventCallback = std::function<void(Channel&)>;

    explicit Channel(EventLoop* loop, int fd);
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel();

    void set_revents(uint32_t revents); // 由 Poller 写入本轮就绪事件，业务代码不应调用
    void handle_events(); // 由 Loop 触发调用

    void set_read_callback(EventCallback cb);
    void set_write_callback(EventCallback cb);
    void set_close_callback(EventCallback cb);
    void set_error_callback(EventCallback cb);

    void tie_to_object(const std::shared_ptr<void>& obj); // 回调期间保活 owner

    void enable_reading();
    void enable_writing();
    void disable_reading();
    void disable_writing();
    void disable_all();

    uint32_t get_events() const;
    bool is_none_event() const;
    bool is_writing() const;
    bool is_reading() const;

    EventLoop* get_owner_loop() const;
    int get_fd() const;

private:
    void update_in_register();
    void remove_in_register();

    void handle_events_with_guard();

    void handle_read_callback();
    void handle_write_callback();
    void handle_close_callback();
    void handle_error_callback();

private:
    static const uint32_t kNoneEvent_;
    static const uint32_t kReadEvent_;
    static const uint32_t kWriteEvent_;

    EventLoop* loop_;                   // 所属 EventLoop，非 owning。

    int fd_;                            // 当前 Channel 绑定的 fd。
    uint32_t events_;                   // 当前感兴趣事件掩码。
    uint32_t revents_;                  // Poller 返回的本轮就绪事件掩码。

    std::weak_ptr<void> tie_;           // 用于在回调期间暂时保活所有者对象。
    bool isTied_;                       // 是否已启用 tie 机制。

    EventCallback readCallback_;        // 读事件回调。
    EventCallback writeCallback_;       // 写事件回调，可选。
    EventCallback closeCallback_;       // 关闭事件回调，可选。
    EventCallback errorCallback_;       // 错误事件回调，可选。
};
