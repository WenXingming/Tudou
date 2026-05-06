// ============================================================================
// Channel.cpp
// fd 事件通道实现，保持“同步兴趣 -> 分发就绪事件”的单层职责。
// ============================================================================

#include "Channel.h"
#include "EventLoop.h"
#include "spdlog/spdlog.h"

#include <sys/epoll.h>
#include <cassert>

const uint32_t Channel::kNoneEvent_ = 0;
const uint32_t Channel::kReadEvent_ = EPOLLIN | EPOLLPRI;
const uint32_t Channel::kWriteEvent_ = EPOLLOUT;

Channel::Channel(EventLoop* loop, int fd)
    : loop_(loop)
    , fd_(fd)
    , events_(kNoneEvent_)
    , revents_(kNoneEvent_)
    , tie_()
    , isTied_(false)
    , readCallback_(nullptr)
    , writeCallback_(nullptr)
    , closeCallback_(nullptr)
    , errorCallback_(nullptr) {

    // 构造时立即注册到 Poller，保证 fd 和 Channel 生命周期严格同步
    loop_->assert_in_loop_thread();
    update_in_register();
}

Channel::~Channel() {
    loop_->assert_in_loop_thread();
    disable_all();
    remove_in_register();
    ::close(fd_); // TODO: 引入 Socket/Fd RAII 负责 close。这里 fd 由上层创建，当前销毁
}

void Channel::set_read_callback(EventCallback cb) {
    readCallback_ = std::move(cb);
}

void Channel::set_write_callback(EventCallback cb) {
    writeCallback_ = std::move(cb);
}

void Channel::set_close_callback(EventCallback cb) {
    closeCallback_ = std::move(cb);
}

void Channel::set_error_callback(EventCallback cb) {
    errorCallback_ = std::move(cb);
}

void Channel::set_revents(uint32_t revents) {
    revents_ = revents;
}

void Channel::handle_events() {
    // 若启用了 tie 机制，必须先保活所有者对象，再进入事件分发。
    if (isTied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard) {
            handle_events_with_guard();
        }
    }
    else {
        handle_events_with_guard();
    }
}

void Channel::tie_to_object(const std::shared_ptr<void>& obj) {
    tie_ = obj;
    isTied_ = true;
}

EventLoop* Channel::get_owner_loop() const {
    return loop_;
}

int Channel::get_fd() const {
    return fd_;
}

void Channel::enable_reading() {
    events_ |= kReadEvent_;
    update_in_register();
}

void Channel::enable_writing() {
    events_ |= kWriteEvent_;
    update_in_register();
}

void Channel::disable_reading() {
    events_ &= ~kReadEvent_;
    update_in_register();
}

void Channel::disable_writing() {
    events_ &= ~kWriteEvent_;
    update_in_register();
}

void Channel::disable_all() {
    events_ = kNoneEvent_;
    update_in_register();
}

bool Channel::is_none_event() const {
    return events_ == kNoneEvent_;
}

bool Channel::is_writing() const {
    return (events_ & kWriteEvent_) != 0;
}

bool Channel::is_reading() const {
    return (events_ & kReadEvent_) != 0;
}

uint32_t Channel::get_events() const {
    return events_;
}

void Channel::update_in_register() {
    loop_->update_channel(this);
}

void Channel::remove_in_register() {
    loop_->remove_channel(this);
}

void Channel::handle_events_with_guard() {
    // 事件分发顺序遵循“关闭/错误优先于正常读写”，避免已失效 fd 继续走业务回调。
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        handle_close_callback();
        return;
    }
    if (revents_ & EPOLLERR) {
        spdlog::error("Channel::handle_events_with_guard(). EPOLLERR on fd: {}", fd_);
        handle_error_callback();
        return;
    }
    if (revents_ & (EPOLLIN | EPOLLPRI)) {
        handle_read_callback();
    }
    if (revents_ & EPOLLOUT) {
        handle_write_callback();
    }
}

void Channel::handle_read_callback() {
    assert(readCallback_ != nullptr);
    readCallback_(*this);
}

void Channel::handle_write_callback() {
    assert(writeCallback_ != nullptr);
    writeCallback_(*this);
}

void Channel::handle_close_callback() {
    assert(closeCallback_ != nullptr);
    closeCallback_(*this);
}

void Channel::handle_error_callback() {
    assert(errorCallback_ != nullptr);
    errorCallback_(*this);
}
