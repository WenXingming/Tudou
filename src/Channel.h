/**
 * @file Channel.h
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 * @details
 *
 * 主要职责：
 *  - 表示某个 fd 对应的“感兴趣事件”（event）和 poller 返回的“发生事件”（revent）。
 *  - 保存该 fd 在可读/可写/关闭/错误等情况下需要触发的回调函数。
 *  - 在事件发生时（EventLoop 从 Poller 得到活动事件并设置 revent），
 *    调用 publish_event，按 revent 分发并触发相应的回调。
 *  - 当对感兴趣事件做修改（enable/disable）时，通过 update_to_register 通知 EventLoop/ Poller 更新注册信息。
 *
 * 重要注意点：
 *  - Channel 不拥有 fd（不负责 close）；仅做事件/回调的绑定与分发。
 *  - Channel 依赖 EventLoop 来完成与 Poller 的交互（注册/更新/删除）。
 *  - 回调以 std::function<void()> 保存，用户需在外部捕获必要上下文（例如 shared_ptr 保持生命周期）。
 *
 * 用法概览：
 *  1. 创建 Channel 并传入所属的 EventLoop 和 fd。
 *  2. subscribe_on_* 注册对应事件的回调函数。
 *  3. 调用 enable_reading/enable_writing 等修改感兴趣事件。
 *  4. EventLoop 在 poller 返回活动 events 时，设置 revent 并调用 publish_event。
 *
 * 核心函数：
 *  - publish_events()：事件发生后进行回调。
 */

#pragma once
#include <functional>
#include <cstdint>
#include <sys/epoll.h>
#include <memory>
#include "../base/NonCopyable.h"

class Timestamp;
class EventLoop;
class Channel : public NonCopyable {
private:
    EventLoop* loop;    // 改变 channel 的 event 后，需要更新 channels 注册中心（在 poller），依赖 loop 完成（依赖注入设计）

    int fd;             // 并不持有，无需负责 close
    uint32_t event;     // interesting events
    uint32_t revent;    // received events types of poller, channel 调用 publish_events 时根据 revent 进行事件分发回调

    std::function<void()> readCallback; // 根据 revents 调用事件的回调函数。没有 master，所以存放在这里
    std::function<void()> writeCallback;
    std::function<void()> closeCallback;
    std::function<void()> errorCallback;

    static const int kNoneEvent;
    static const int kReadEvent;
    static const int kWriteEvent;

public:
    explicit Channel(EventLoop* loop, int fd, uint32_t event, uint32_t revent,
        std::function<void()> readCallback,
        std::function<void()> writeCallback,
        std::function<void()> closeCallback,
        std::function<void()> errorCallback);

    ~Channel();

    // 核心函数，事件发生后进行回调。何时被调用：EventLoop 事件循环中被调用（先通过 poller 返回 active channels）
    void publish_events(Timestamp receiveTime);

    // 没有办法，没有 master 注册中心，只能把订阅函数 callback 存在本类里，所以也需要提供公开接口
    // 这里好像还真不好用注册中心，因为 Acceptor、TcpConnection 对于 readCallback 传入的回调函数不同
    void subscribe_on_read(std::function<void()> cb);
    void subscribe_on_write(std::function<void()> cb);
    void subscribe_on_close(std::function<void()> cb);
    void subscribe_on_error(std::function<void()> cb);

    int get_fd() const;
    uint32_t get_event() const;

    // 设置 fd 感兴趣的事件
    void enable_reading();
    void disable_reading();
    void enable_writing();
    void disable_writing();
    void disable_all();

    // poller 监听到事件后设置此值
    void set_revent(uint32_t _revent);

    // 当改变 channel 的 event 后，需要在 poller 里面更改（更新） epoll fd。何时被调用：调用 enable_reading 等函数改变 event 后
    // 当创建 channel 后，需要在 poller 里面更新 channels
    void update_to_register();
    void remove_in_register();

private:
    // 根据事件调用回调函数。何时被调用：被 publish_events() 调用
    void publish_events_with_guard(Timestamp receiveTime);

    // 事件发生了就需要 publish。没有 master 注册中心，所以发布时直接本地自己触发回调 callback
    void publish_read();
    void publish_write();
    void publish_close();
    void publish_error();
};
