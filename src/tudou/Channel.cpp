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
    update_in_register(); // 创建 channel 后立即注册到 poller 上，保证 epoller 访问 channel 有效（生命周期）
}

Channel::~Channel() {
    // fd 生命期应该由 Channel 管理（虽然是上层创建，但是销毁应该由 Channel 负责）。这样就做到了 Channel 完全封装 fd，且 Epoller 的 Channels 和 fd 同步
    // 注销 channel，channels 和 负责和 Epoller 同步（相邻类）。该同步不再交给上层 Acceptor / TcpConnection 负责
    // 好处是：保证了 poller 访问 channel 有效（生命周期），当 channel 析构时才 unregister，因此只要还在 register，Epoller 就一定有效访问 channel。
    disable_all();
    remove_in_register();
    ::close(fd);
}

int Channel::get_fd() const {
    return fd;
}

void Channel::enable_reading() {
    this->event |= Channel::kReadEvent;
    update_in_register(); // 必须调用，否则 poller 不知道 event 变化。主要是使用 epoll_ctl 更新 epollFd 上的事件
}

void Channel::enable_writing() {
    event |= kWriteEvent;
    update_in_register();
}

void Channel::disable_reading() {
    this->event &= ~Channel::kReadEvent;
    update_in_register();
}

void Channel::disable_writing() {
    event &= ~kWriteEvent;
    update_in_register();
}

void Channel::disable_all() {
    event = kNoneEvent;
    update_in_register();
}

uint32_t Channel::get_event() const {
    return event;
}

// poller 监听到事件后设置此值
void Channel::set_revent(uint32_t _revent) {
    revent = _revent;
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

// channel 借助依赖注入的 EventLoop 完成在 Poller 的注册、更新、删除操作
void Channel::update_in_register() {
    loop->update_channel(this);
}

void Channel::remove_in_register() {
    loop->remove_channel(this);
}

void Channel::handle_events() {
    handle_events_with_guard();
}

// 断开连接的处理并不简单：对方关闭连接，会触发 Channel::handle_event()，后者调用 handle_close()。
// handle_close() 调用上层注册的 closeCallback，TcpConnection::close_callback()。
// TcpConnection::close_callback() 负责关闭连接，在 TcpServer 中销毁 TcpConnection 对象。此时 Channel 对象也会被销毁
// 然而此时 handle_events_with_guard() 还没有返回，后续代码继续执行，可能访问已经被销毁的 Channel 对象，导致段错误
// 见书籍 p274。muduo 的做法是通过 Channel::tie() 绑定一个弱智能指针，延长其生命周期，保证 Channel 对象在 handle_events_with_guard() 执行期间不会被销毁
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
    this->readCallback(*this);
}

void Channel::handle_write() {
    assert(this->writeCallback != nullptr);
    this->writeCallback(*this);
}

void Channel::handle_close() {
    assert(this->closeCallback != nullptr);
    this->closeCallback(*this);
}

void Channel::handle_error() {
    assert(this->errorCallback != nullptr);
    this->errorCallback(*this);
}
