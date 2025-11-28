/**
 * @file Channel.h
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 * @details
 *
 * 主要职责：
 *  - 表示某个 fd 对应的“感兴趣事件”（event）和 poller
 * 返回的“发生事件”（revent）。
 *  - 保存该 fd 在可读/可写/关闭/错误等情况下需要触发的回调函数。
 *  - 在事件发生时（EventLoop 从 Poller 得到活动事件并设置 revent），
 *    调用 handle_events，按 revent 分发并触发相应的回调。
 *  - 当对感兴趣事件做修改（enable/disable）时，通过 update_to_register 通知
 * EventLoop/ Poller 更新注册信息。
 *
 * 重要注意点：
 *  - Channel 不拥有 fd（不负责 close）；仅做事件/回调的绑定与分发。
 *  - Channel 依赖 EventLoop 来完成与 Poller 的交互（注册/更新/删除）。
 *  - 回调以 std::function<void()> 保存，用户需在外部捕获必要上下文（例如
 * shared_ptr 保持生命周期）。
 *
 * 用法概览：
 *  1. 创建 Channel 并传入所属的 EventLoop 和 fd。
 *  2. set_*_callback 注册对应事件的回调函数。
 *  3. 调用 enable_reading/enable_writing 等修改感兴趣事件。
 *  4. EventLoop 在 poller 返回活动 events 时，设置 revent 并调用 handle_events。
 *
 * 核心函数：
 *  - handle_events()：事件发生后进行回调。
 */

#pragma once
#include <cstdint>
#include <functional>
#include <sys/epoll.h>

#include "../base/NonCopyable.h"

class Timestamp;
class EventLoop;
class Channel : public NonCopyable {
public:
    explicit Channel(EventLoop* loop, int fd);
    ~Channel();

    int get_fd() const;

    void enable_reading();
    void enable_writing();
    void disable_reading();
    void disable_writing();
    void disable_all();
    uint32_t get_event() const;

    void set_revent(uint32_t _revent);

    // 注册回调函数
    void set_read_callback(std::function<void()> cb);
    void set_write_callback(std::function<void()> cb);
    void set_close_callback(std::function<void()> cb);
    void set_error_callback(std::function<void()> cb);

    // channel 借助依赖注入的 EventLoop 完成在 Poller 的注册、更新、删除操作
    void update_to_register();
    void remove_in_register();

    // 核心函数，事件发生后进行回调
    void handle_events(Timestamp receiveTime);

private:
    void handle_events_with_guard(Timestamp receiveTime);
    void handle_read();
    void handle_write();
    void handle_close();
    void handle_error();

private:
    static const uint32_t kNoneEvent;
    static const uint32_t kReadEvent;
    static const uint32_t kWriteEvent;

    EventLoop* loop;                // 依赖注入
    int fd;                         // 并不持有，无需负责 close。为了同步，因此 channel 不负责 remove_in_register
    uint32_t event{ kNoneEvent };   // interesting events
    uint32_t revent{ kNoneEvent };  // received events types of poller, channel 调用
    std::function<void()> readCallback{ nullptr };
    std::function<void()> writeCallback{ nullptr };
    std::function<void()> closeCallback{ nullptr };
    std::function<void()> errorCallback{ nullptr };
};
