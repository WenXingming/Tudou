/**
 * @file Channel.h
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象，可以理解为 fd + 事件 + 回调 几者的结合体
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
 *
 * 主要职责：
 *  - 表示某个 fd 对应的 “感兴趣事件”（event）和 poller 返回的 “发生事件”（revent）。
 *  - 保存该 fd 在可读/可写/关闭/错误等情况下需要触发的回调函数。
 *  - 在事件发生时（Poller 得到活动 channels 并设置 revent），对活动的 channels 调用 handle_events，按 revent 分发并触发相应的回调。
 *  - 当对感兴趣事件做修改（enable/disable）时，通过 update_in_register 通知 EventLoop / Poller 更新注册信息。
 */

#pragma once

#include <functional>
#include <memory>

class EventLoop; // 前向声明，减少头文件依赖
class Channel {
    // or: typedef std::function<void(Channel&)> ReadEventCallback;
    using ReadEventCallback = std::function<void(Channel&)>;
    using WriteEventCallback = std::function<void(Channel&)>;
    using CloseEventCallback = std::function<void(Channel&)>;
    using ErrorEventCallback = std::function<void(Channel&)>;

private:
    static const uint32_t kNoneEvent;
    static const uint32_t kReadEvent;
    static const uint32_t kWriteEvent;

    EventLoop* loop;                    // 依赖注入
    int fd;                             // channel 负责管理的 fd
    uint32_t events;                    // interesting events
    uint32_t revents;                   // received events
    int index;                          // 暂时不知道作用，先保留

    std::weak_ptr<void> tie;            // 绑定一个弱智能指针，延长其生命周期，防止 handle_events_with_guard 过程中被销毁。void 因为下层不需要知道上层类型
    bool isTied;                        // Acceptor 不需要 tie，TcpConnection 需要 tie (shared_ptr, shared_from_this)

    ReadEventCallback readCallback;     // 回调函数接口。执行上层逻辑，回调函数的参数由下层传入
    WriteEventCallback writeCallback;
    CloseEventCallback closeCallback;
    ErrorEventCallback errorCallback;

public:
    explicit Channel(EventLoop* loop, int fd);
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;
    ~Channel();

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

    void set_revents(uint32_t _revents);  // poller 监听到事件后调用此函数设置 revents

    void set_index(int _idx);
    int get_index() const;

    // Tie this channel to the owner object managed by shared_ptr
    // This prevents the owner object being destroyed in handle_event (lengthen its lifetime)
    void tie_to_object(const std::shared_ptr<void>& obj);

    // 上层注入回调函数的接口
    void set_read_callback(ReadEventCallback _cb);
    void set_write_callback(WriteEventCallback _cb);
    void set_close_callback(CloseEventCallback _cb);
    void set_error_callback(ErrorEventCallback _cb);

    // 核心函数：事件发生后调用回调
    void handle_events();

private:
    // 内部属性改变时，需要在 poller 上更新（epoll）。该方法由 channel 完成（channel 负责和相邻类 Epoller 同步）
    void update_in_register();
    void remove_in_register();

    void handle_events_with_guard();

    void handle_read_callback();
    void handle_write_callback();
    void handle_close_callback();
    void handle_error_callback();
};
