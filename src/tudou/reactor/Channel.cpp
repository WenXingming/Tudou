// ============================================================================
// Channel.cpp
// fd 事件通道实现，保持“同步兴趣 -> 分发就绪事件”的单层职责。
// ============================================================================

#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"

#include <sys/epoll.h>
#include <cassert>
#include <spdlog/spdlog.h>

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

    assert(loop_->is_in_loop_thread());
    // 构造时立即注册到 Poller，保证 Channel 生命周期内始终受 EventLoop 管理，二者严格同步绑定。
    // 或者采用惰性注册（Lazy Registration），避免多支付一次昂贵的 `epoll_ctl` 系统调用
    // update_in_register();
}

Channel::~Channel() {
    // 析构时立即从 Poller 注销，保证 Channel 生命周期内始终受 EventLoop 管理，二者严格同步绑定。
    assert(loop_->is_in_loop_thread());
    disable_all();
    remove_in_register();
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
    if (!isTied_) {
        handle_events_with_guard(); // Acceptor 走此分支，无需保活
        return;
    }

    std::shared_ptr<void> guard = tie_.lock(); // 升级：栈上临时强引用
    if (guard) {
        handle_events_with_guard(); // 对象保活，安全执行事件分发
    }
    else; // 返回 null 是 tie_to_object 后对象被析构的正常结果，不需要报错
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
    // EPOLLHUP | EPOLLIN 同时到来时（常见于 read 返回 0 即 EOF 场景），走 read 逻辑更合理，避免误判为异常。EPOLLHUP 单独到来时才走 close 逻辑。
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        handle_close_callback();
        return;
    }
    if (revents_ & EPOLLERR) {
        handle_error_callback();
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
    if (!writeCallback_) {
        return;
    }
    writeCallback_(*this);
}

void Channel::handle_close_callback() {
    if (!closeCallback_) {
        return;
    }
    closeCallback_(*this);
}

void Channel::handle_error_callback() {
    if (!errorCallback_) {
        return;
    }
    errorCallback_(*this);
}
