/**
 * @file Channel.h
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象。
 * @author wenxingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "Channel.h"
#include "EventLoop.h"
#include "../base/Timestamp.h"
#include "../base/Log.h"
#include <sys/epoll.h>

const int Channel::kNoneEvent = 0;
const int Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const int Channel::kWriteEvent = EPOLLOUT;

// 构造函数。把 fd、event、revent、readCallback 等都设置为参数可能更好，避免使用时忘记设置（提醒使用者）
Channel::Channel(EventLoop* _loop, int _fd, uint32_t _event, uint32_t _revent,
    std::function<void()> _readCallback, std::function<void()> _writeCallback,
    std::function<void()> _closeCallback, std::function<void()> _errorCallback)
    : loop(_loop)
    , fd(_fd)
    , event(_event)
    , revent(_revent)
    , readCallback(std::move(_readCallback))
    , writeCallback(std::move(_writeCallback))
    , closeCallback(std::move(_closeCallback))
    , errorCallback(std::move(_errorCallback)) {

    // 更新到 poller 的 channels。构造函数和析构函数太好用了，无需记忆和害怕忘记调用， RAII 就是好
    update_to_register();
}

Channel::~Channel() {
    // 析构时从 poller 的 channels 中删除。构造函数和析构函数太好用了，无需记忆和害怕忘记调用， RAII 就是好
    // 不可以在析构函数中调用，channels 本来就是持有智能指针！
    // remove_in_register();
}

void Channel::handle_events(Timestamp receiveTime) {
    handle_events_with_guard(receiveTime);
}

void Channel::handle_events_with_guard(Timestamp receiveTime) {
    LOG::LOG_DEBUG("poller find event, channel handle event: %d", revent);

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

void Channel::enable_reading() {
    this->event |= Channel::kReadEvent;
    update_to_register(); // 必须调用，因为需要维护 epoll fd。避免外部忘记调用
}

void Channel::disable_reading() {
    this->event &= ~Channel::kReadEvent;
    update_to_register();
}

void Channel::enable_writing() {
    event |= kWriteEvent;
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

void Channel::set_revent(uint32_t _revent) {
    revent = _revent;
}

void Channel::update_to_register() {
    loop->update_channel(this);
}

void Channel::remove_in_register() {
    loop->remove_channel(this);
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

int Channel::get_fd() const {
    return fd;
}

uint32_t Channel::get_event() const {
    return event;
}

void Channel::handle_read() {
    if (this->readCallback) {
        this->readCallback();
    }
    else {
        LOG::LOG_ERROR("Channel::handle_read(). no readCallback.");
    }
}

void Channel::handle_write() {
    if (this->writeCallback) {
        this->writeCallback();
    }
    else {
        LOG::LOG_ERROR("Channel::handle_write(). no writeCallback.");
    }
}

void Channel::handle_close() {
    if (this->closeCallback) {
        this->closeCallback();
    }
    else {
        LOG::LOG_ERROR("Channel::handle_close(). no closeCallback.");
    }
}

void Channel::handle_error() {
    if (this->errorCallback) {
        this->errorCallback();
    }
    else {
        LOG::LOG_ERROR("Channel::handle_error(). no errorCallback.");
    }
}
