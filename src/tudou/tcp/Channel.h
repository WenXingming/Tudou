/**
 * @file Channel.h
 * @brief fd + 事件 + 回调 的封装，负责事件分发与 Poller 注册同步。
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * 职责：绑定 fd 的感兴趣事件（events）与回调函数，在 Poller 返回就绪事件后调用 handle_events() 按事件类型分发回调。
 * enable/disable 系列方法会自动通过 EventLoop 同步到 Poller。
 *
 * 线程安全：所有方法须在所属 EventLoop 线程调用。
 */

#pragma once
#include <functional>
#include <memory>

class EventLoop;
class Channel {
public:
    using EventCallback = std::function<void(Channel&)>;

public:
    explicit Channel(EventLoop* loop, int fd);
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel();

    // 上层注入回调函数的接口
    void set_read_callback(EventCallback cb);
    void set_write_callback(EventCallback cb);
    void set_close_callback(EventCallback cb);
    void set_error_callback(EventCallback cb);

    // poller 监听到事件后调用此函数设置 revents
    void set_revents(uint32_t revents);

    // 核心函数：事件发生后调用回调
    void handle_events();

    // 绑定所有者的 shared_ptr，防止回调执行期间对象被析构（TcpConnection 使用），延长其生命周期
    void tie_to_object(const std::shared_ptr<void>& obj);

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
    void update_in_register(); // 同步事件变更到 Poller
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

    EventLoop* loop_;                    // 依赖注入，所属 EventLoop 指针（非 owning）
    int fd_;                             // Channel 绑定的 fd
    uint32_t events_;                    // 感兴趣事件
    uint32_t revents_;                   // Poller 返回的就绪事件

    std::weak_ptr<void> tie_;            // 防止回调期间所有者被析构，通过 tie_ 锁定所有者，延长其生命周期
    bool isTied_;                        // 是否启用 tie 机制（Acceptor 不需要，TcpConnection 需要）

    // 回调函数对象，参数为下层 Channel 对象，回调函数逻辑由上层实现并注入到下层 Channel 对象
    EventCallback readCallback_;
    EventCallback writeCallback_;
    EventCallback closeCallback_;
    EventCallback errorCallback_;
};
