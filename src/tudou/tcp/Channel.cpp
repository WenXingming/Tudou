/**
 * @file Channel.cpp
 * @brief Channel 用于把 IO 事件与回调绑定起来的抽象，可以理解为 fd + 事件 + 回调 几者的结合体
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 */

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
    , index_(-1)
    , tie_()
    , isTied_(false)
    , readCallback_(nullptr)
    , writeCallback_(nullptr)
    , closeCallback_(nullptr)
    , errorCallback_(nullptr) {


    // - channel 负责和 epoller 同步（相邻类），不再交给上层 Acceptor / TcpConnection 负责，它们应该感知不到 poller 的存在
    // - 创建 channel 后立即通过构造函数注册到 poller 上，严格同步 fd 和 channel 保证了 epoller 访问 channel 有效（生命周期）
    // - channel（TcpConnection）的创建设计是在 ioLoop 线程（负责） ，观察 TcpServer 中代码其实无需 run_in_loop 但为了保险
    loop_->assert_in_loop_thread();
    this->update_in_register();
}

Channel::~Channel() {

    // fd 生命期应该由 Channel 管理（虽然是上层创建，但是销毁应该由 Channel 负责）。这样就做到了 Channel 完全封装 fd，且 Epoller 的 Channels 和 fd 同步
    // 注销 channel，channel 负责 channels 和 Epoller 同步（相邻类）。该同步不再交给上层 Acceptor / TcpConnection 负责
    // 好处是：保证了 poller 访问 channel 有效（生命周期），当 channel 析构时才 unregister，因此只要还在 register，Epoller 就一定有效访问 channel。

    loop_->assert_in_loop_thread();
    disable_all();
    remove_in_register();
    ::close(fd_); // TODO: 引入 Socket/Fd RAII 负责 close。TcpConnection/Acceptor 组合持有 Socket+Channel
}

void Channel::set_read_callback(ReadEventCallback cb) {
    this->readCallback_ = std::move(cb);
}

void Channel::set_write_callback(WriteEventCallback cb) {
    this->writeCallback_ = std::move(cb);
}

void Channel::set_close_callback(CloseEventCallback cb) {
    this->closeCallback_ = std::move(cb);
}

void Channel::set_error_callback(ErrorEventCallback cb) {
    this->errorCallback_ = std::move(cb);
}

void Channel::set_revents(uint32_t revents) {
    this->revents_ = revents;
}

void Channel::handle_events() {
    // 断开连接的处理并不简单：对方关闭连接，会触发 Channel::handle_event()，后者调用 handle_close_callback()。
    // handle_close_callback() 调用上层注册的 closeCallback，TcpConnection::close_callback().
    // TcpConnection::close_callback() 负责关闭连接，在 TcpServer 中销毁 TcpConnection 对象。此时 Channel 对象也会被销毁
    // 然而此时 handle_events_with_guard() 还没有返回，后续代码继续执行，可能访问已经被销毁的 Channel 对象，导致段错误
    // 见书籍 p274。muduo 的做法是通过 Channel::tie() 绑定一个弱智能指针，延长其生命周期，保证 Channel 对象在 handle_events_with_guard() 执行期间不会被销毁


    // 通过弱智能指针 tie 绑定一个 shared_ptr 智能指针，延长其生命周期，防止 handle_events_with_guard 过程中被销毁
    // 只有对象是通过 shared_ptr 管理的，才能锁定。所以需要 isTied 标志
    // Accetor 不需要 tie（因为没有 remove 回调，而且也不是 shared_ptr 管理的）；TcpConnection 需要 tie，其有 remove 回调，且是 shared_ptr 管理的

    if (isTied_) {
        std::shared_ptr<void> guard = tie_.lock();
        if (guard) {
            this->handle_events_with_guard();
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
    this->events_ |= Channel::kReadEvent_;
    this->update_in_register(); // 必须调用，否则 poller 不知道 event 变化。主要是使用 epoll_ctl 更新 epollFd 上的事件
}

void Channel::enable_writing() {
    this->events_ |= Channel::kWriteEvent_;
    this->update_in_register();
}

void Channel::disable_reading() {
    this->events_ &= ~Channel::kReadEvent_;
    this->update_in_register();
}

void Channel::disable_writing() {
    this->events_ &= ~Channel::kWriteEvent_;
    this->update_in_register();
}

void Channel::disable_all() {
    this->events_ = Channel::kNoneEvent_;
    this->update_in_register();
}

bool Channel::is_none_event() const {
    return this->events_ == Channel::kNoneEvent_;
}

bool Channel::is_writing() const {
    return (this->events_ & Channel::kWriteEvent_) != 0;
}

bool Channel::is_reading() const {
    return (this->events_ & Channel::kReadEvent_) != 0;
}

uint32_t Channel::get_events() const {
    return this->events_;
}

void Channel::set_index(int idx) {
    this->index_ = idx;
}

int Channel::get_index() const {
    return this->index_;
}

void Channel::update_in_register() {
    loop_->update_channel(this);
}

void Channel::remove_in_register() {
    loop_->remove_channel(this);
}

void Channel::handle_events_with_guard() {
    if ((revents_ & EPOLLHUP) && !(revents_ & EPOLLIN)) {
        this->handle_close_callback();
        return;
    }
    if (revents_ & (EPOLLERR)) {
        spdlog::error("Channel::handle_events_with_guard(). EPOLLERR on fd: {}", fd_);
        this->handle_error_callback();
        return;
    }
    if (revents_ & (EPOLLIN | EPOLLPRI)) {
        this->handle_read_callback();
    }
    if (revents_ & EPOLLOUT) {
        this->handle_write_callback();
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
