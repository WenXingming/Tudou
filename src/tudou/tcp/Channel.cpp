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

const uint32_t Channel::kNoneEvent = 0;
const uint32_t Channel::kReadEvent = EPOLLIN | EPOLLPRI;
const uint32_t Channel::kWriteEvent = EPOLLOUT;

Channel::Channel(EventLoop* _loop, int _fd)
    : loop(_loop)
    , fd(_fd)
    , events(kNoneEvent)
    , revents(kNoneEvent)
    , index(-1)
    , tie()
    , isTied(false)
    , readCallback(nullptr)
    , writeCallback(nullptr)
    , closeCallback(nullptr)
    , errorCallback(nullptr) {

    /*
    - channel 负责和 epoller 同步（相邻类），不再交给上层 Acceptor / TcpConnection 负责，它们应该感知不到 poller 的存在
    - 创建 channel 后立即通过构造函数注册到 poller 上，严格同步 fd 和 channel 保证了 epoller 访问 channel 有效（生命周期）
    - channel（TcpConnection）的创建设计是在 ioLoop 线程（负责） ，观察 TcpServer 中代码其实无需 run_in_loop 但为了保险
    */
    loop->run_in_loop([this]() {
        this->update_in_register();
        });
}

Channel::~Channel() {

    /*
    fd 生命期应该由 Channel 管理（虽然是上层创建，但是销毁应该由 Channel 负责）。这样就做到了 Channel 完全封装 fd，且 Epoller 的 Channels 和 fd 同步
    注销 channel，channel 负责 channels 和 Epoller 同步（相邻类）。该同步不再交给上层 Acceptor / TcpConnection 负责
    好处是：保证了 poller 访问 channel 有效（生命周期），当 channel 析构时才 unregister，因此只要还在 register，Epoller 就一定有效访问 channel。
    */
    loop->assert_in_loop_thread();
    disable_all();
    remove_in_register();
    ::close(fd);
}

EventLoop* Channel::get_owner_loop() const {
    return loop;
}

int Channel::get_fd() const {
    return fd;
}

void Channel::enable_reading() {
    this->events |= Channel::kReadEvent;
    update_in_register(); // 必须调用，否则 poller 不知道 event 变化。主要是使用 epoll_ctl 更新 epollFd 上的事件
}

void Channel::enable_writing() {
    events |= kWriteEvent;
    update_in_register();
}

void Channel::disable_reading() {
    events &= ~kReadEvent;
    update_in_register();
}

void Channel::disable_writing() {
    events &= ~kWriteEvent;
    update_in_register();
}

void Channel::disable_all() {
    events = kNoneEvent;
    update_in_register();
}

bool Channel::is_none_event() const {
    return events == kNoneEvent;
}

bool Channel::is_writing() const {
    return (events & kWriteEvent);
}

bool Channel::is_reading() const {
    return (events & kReadEvent);
}

uint32_t Channel::get_events() const {
    return events;
}

void Channel::set_revents(uint32_t _revents) {
    revents = _revents;
}

void Channel::set_index(int _idx) {
    index = _idx;
}

int Channel::get_index() const {
    return index;
}

void Channel::tie_to_object(const std::shared_ptr<void>& obj) {
    tie = obj;
    isTied = true;
}

void Channel::set_read_callback(ReadEventCallback _cb) {
    this->readCallback = std::move(_cb);
}

void Channel::set_write_callback(WriteEventCallback _cb) {
    this->writeCallback = std::move(_cb);
}

void Channel::set_close_callback(CloseEventCallback _cb) {
    this->closeCallback = std::move(_cb);
}

void Channel::set_error_callback(ErrorEventCallback _cb) {
    this->errorCallback = std::move(_cb);
}

void Channel::update_in_register() {
    loop->update_channel(this);
}

void Channel::remove_in_register() {
    loop->remove_channel(this);
}

void Channel::handle_events() {
    /*
    断开连接的处理并不简单：对方关闭连接，会触发 Channel::handle_event()，后者调用 handle_close_callback()。
    handle_close_callback() 调用上层注册的 closeCallback，TcpConnection::close_callback().
    TcpConnection::close_callback() 负责关闭连接，在 TcpServer 中销毁 TcpConnection 对象。此时 Channel 对象也会被销毁
    然而此时 handle_events_with_guard() 还没有返回，后续代码继续执行，可能访问已经被销毁的 Channel 对象，导致段错误
    见书籍 p274。muduo 的做法是通过 Channel::tie() 绑定一个弱智能指针，延长其生命周期，保证 Channel 对象在 handle_events_with_guard() 执行期间不会被销毁
    */
    /*
    通过弱智能指针 tie 绑定一个 shared_ptr 智能指针，延长其生命周期，防止 handle_events_with_guard 过程中被销毁
    只有对象是通过 shared_ptr 管理的，才能锁定。所以需要 isTied 标志
    Accetor 不需要 tie（因为没有 remove 回调，而且也不是 shared_ptr 管理的）；TcpConnection 需要 tie，其有 remove 回调，且是 shared_ptr 管理的
    */
    if (isTied) {
        std::shared_ptr<void> guard = tie.lock();
        if (guard) {
            this->handle_events_with_guard();
        }
    }
    else {
        handle_events_with_guard();
    }
}

void Channel::handle_events_with_guard() {
    if ((revents & EPOLLHUP) && !(revents & EPOLLIN)) {
        this->handle_close_callback();
        return;
    }
    if (revents & (EPOLLERR)) {
        spdlog::error("Channel::handle_events_with_guard(). EPOLLERR on fd: {}", this->fd);
        this->handle_error_callback();
        return;
    }
    if (revents & (EPOLLIN | EPOLLPRI)) {
        this->handle_read_callback();
    }
    if (revents & EPOLLOUT) {
        this->handle_write_callback();
    }
}

void Channel::handle_read_callback() {
    assert(this->readCallback != nullptr);
    this->readCallback(*this);
}

void Channel::handle_write_callback() {
    assert(this->writeCallback != nullptr);
    this->writeCallback(*this);
}

void Channel::handle_close_callback() {
    assert(this->closeCallback != nullptr);
    this->closeCallback(*this);
}

void Channel::handle_error_callback() {
    assert(this->errorCallback != nullptr);
    this->errorCallback(*this);
}
