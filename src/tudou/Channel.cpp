/**
 * @file Channel.h
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "Channel.h"

#include <sys/epoll.h>
#include <assert.h>

#include "../base/Log.h"
#include "../base/Timestamp.h"
#include "EventLoop.h"

const uint32_t Channel::kNoneEvent = 0;
const uint32_t Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const uint32_t Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* _loop, int _fd) : loop(_loop), fd(_fd) {
    // 不在构造函数里调用。上层在创建 fd 时调用，上层保证 channels、fd 同步
    // update_to_register();
}

Channel::~Channel() {
    // 不在析构函数里调用？上层在销毁 fd 时调用，上层保证 channels、fd 同步
    // remove_in_register();
}

int Channel::get_fd() const {
    return fd;
}

void Channel::enable_reading() {
    this->event |= Channel::kReadEvent;
    update_to_register(); // 必须调用，否则 poller 不知道 event 变化
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

void Channel::set_revent(uint32_t _revent) {
    revent = _revent;
}

void Channel::handle_events(Timestamp receiveTime) {
    handle_events_with_guard(receiveTime);
}

void Channel::update_to_register() {
    loop->update_channel(this);
}

void Channel::remove_in_register() {
    loop->remove_channel(this);
}

void Channel::handle_events_with_guard(Timestamp receiveTime) {
    // LOG::LOG_DEBUG("poller find event, channel handle event: %d", revent);

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
