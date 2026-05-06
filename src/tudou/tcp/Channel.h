// ============================================================================
// Channel.h
// fd 事件通道，负责同步感兴趣事件并把就绪事件分发给回调。
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

    /**
     * @brief 设置读事件回调。
     * @param cb 读事件触发时调用的回调。
     */
    void set_read_callback(EventCallback cb);

    /**
     * @brief 设置写事件回调。
     * @param cb 写事件触发时调用的回调。
     */
    void set_write_callback(EventCallback cb);

    /**
     * @brief 设置关闭事件回调。
     * @param cb 关闭事件触发时调用的回调。
     */
    void set_close_callback(EventCallback cb);

    /**
     * @brief 设置错误事件回调。
     * @param cb 错误事件触发时调用的回调。
     */
    void set_error_callback(EventCallback cb);

    /**
     * @brief 写入 Poller 返回的就绪事件集合。
     * @param revents 本轮就绪事件掩码。
     */
    void set_revents(uint32_t revents);

    /**
     * @brief 分发本轮就绪事件，是 Channel 的唯一执行入口。
     */
    void handle_events();

    /**
     * @brief 绑定所有者对象，防止回调期间对象被析构。
     * @param obj 需要延长生命周期的所有者对象。
     */
    void tie_to_object(const std::shared_ptr<void>& obj);

    /**
     * @brief 获取所属 EventLoop。
     * @return 当前 Channel 所属 EventLoop 指针。
     */
    EventLoop* get_owner_loop() const;

    /**
     * @brief 获取当前绑定的 fd。
     * @return 当前 fd。
     */
    int get_fd() const;

    /**
     * @brief 开启读事件关注。
     */
    void enable_reading();

    /**
     * @brief 开启写事件关注。
     */
    void enable_writing();

    /**
     * @brief 关闭读事件关注。
     */
    void disable_reading();

    /**
     * @brief 关闭写事件关注。
     */
    void disable_writing();

    /**
     * @brief 关闭所有事件关注。
     */
    void disable_all();

    /**
     * @brief 判断当前是否未关注任何事件。
     * @return true 表示当前没有事件兴趣。
     */
    bool is_none_event() const;

    /**
     * @brief 判断当前是否关注写事件。
     * @return true 表示当前包含写事件兴趣。
     */
    bool is_writing() const;

    /**
     * @brief 判断当前是否关注读事件。
     * @return true 表示当前包含读事件兴趣。
     */
    bool is_reading() const;

    /**
     * @brief 获取当前事件兴趣掩码。
     * @return 当前 events 掩码。
     */
    uint32_t get_events() const;

private:
    /**
     * @brief 将当前事件兴趣同步到 Poller。
     */
    void update_in_register();

    /**
     * @brief 将当前 Channel 从 Poller 中移除。
     */
    void remove_in_register();

    /**
     * @brief 在 tie 保护已经建立后分发就绪事件。
     */
    void handle_events_with_guard();

    /**
     * @brief 触发读事件回调。
     */
    void handle_read_callback();

    /**
     * @brief 触发写事件回调。
     */
    void handle_write_callback();

    /**
     * @brief 触发关闭事件回调。
     */
    void handle_close_callback();

    /**
     * @brief 触发错误事件回调。
     */
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
