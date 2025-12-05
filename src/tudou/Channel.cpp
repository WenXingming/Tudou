/**
 * @file Channel.h
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "Channel.h"

#include <cassert>
#include <sys/epoll.h>

#include "spdlog/spdlog.h"
#include "EventLoop.h"


const uint32_t Channel::kNoneEvent = 0;
const uint32_t Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const uint32_t Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* _loop, int _fd)
    : loop(_loop)
    , fd(_fd) {
    update_to_register(); // 创建 channel 后立即注册到 poller 上，和 fd "创建" 同步
}

Channel::~Channel() {
    disable_all();
    remove_in_register(); // 注销 channel，channels 和 负责和 Epoller 同步（相邻类）。该同步不再交给上层 Acceptor / TcpConnection 负责
    ::close(fd); // fd 生命期应该由 Channel 管理（虽然是上层创建，但是销毁应该由 Channel 负责）。这样就做到了 Channel 完全封装 fd，且 Epoller 的 Channels 和 fd 同步
}

int Channel::get_fd() const {
    return fd;
}

void Channel::enable_reading() {
    this->event |= Channel::kReadEvent;
    update_to_register(); // 必须调用，否则 poller 不知道 event 变化。主要是使用 epoll_ctl 更新 epollFd 上的事件
}

void Channel::enable_writing() {
    event |= kWriteEvent;
    update_to_register();
}

void Channel::disable_reading() {
    this->event &= ~Channel::kReadEvent;
    update_to_register();
}

void Channel::disable_writing() {
    event &= ~kWriteEvent;
    update_to_register();
}

void Channel::disable_all() {
    event = kNoneEvent;
    update_to_register();
}

uint32_t Channel::get_event() const {
    return event;
}

// poller 监听到事件后设置此值
void Channel::set_revent(uint32_t _revent) {
    revent = _revent;
}

void Channel::set_read_callback(std::function<void()> cb) {
    this->readCallback = std::move(cb);
}

void Channel::set_write_callback(std::function<void()> cb) {
    this->writeCallback = std::move(cb);
}

void Channel::set_close_callback(std::function<void()> cb) {
    this->closeCallback = std::move(cb);
}

void Channel::set_error_callback(std::function<void()> cb) {
    this->errorCallback = std::move(cb);
}

// channel 借助依赖注入的 EventLoop 完成在 Poller 的注册、更新、删除操作
void Channel::update_to_register() {
    loop->update_channel(this);
}

void Channel::remove_in_register() {
    loop->remove_channel(this);
}

void Channel::handle_events() {
    handle_events_with_guard();
}

void Channel::handle_events_with_guard() {
    spdlog::debug("poller find event, channel handle event: {}", revent);

    if ((revent & EPOLLHUP) && !(revent & EPOLLIN)) {
        this->handle_close();
        return;
    }
    if (revent & (EPOLLERR)) {
        this->handle_error();
        return;
    }
    if (revent & (EPOLLIN | EPOLLPRI)) {
        this->handle_read();
    }
    if (revent & EPOLLOUT) {
        this->handle_write();
    }
}

void Channel::handle_read() {
    assert(this->readCallback != nullptr);
    this->readCallback();
}

void Channel::handle_write() {
    assert(this->writeCallback != nullptr);
    this->writeCallback();
}

void Channel::handle_close() {
    assert(this->closeCallback != nullptr);
    this->closeCallback();
}

void Channel::handle_error() {
    assert(this->errorCallback != nullptr);
    this->errorCallback();
}
