# TcpConnection 发送与关闭设计：线程安全分发、优雅关闭与生命周期解耦

本文说明 `TcpConnection::send()` 如何保证跨线程调用安全，以及当前 `close_connection()` 为什么使用 `shutdown_write()` 和 `disable_all()`。

# Situation — 情境

`TcpConnection` 既被 I/O 线程使用，也可能被业务线程调用：

- 业务线程可能在处理完成后调用 `send()`；
- 连接可能同时因为对端关闭、错误或服务器停止而进入关闭流程；
- `TcpConnection` 对象的 C++ 生命周期和底层 fd 生命周期不一定同时结束。

因此，发送路径需要解决线程归属，关闭路径需要同时考虑 TCP 行为和对象生命周期。

# Task — 任务

设计需要保证：

1. socket 和写缓冲只在所属 I/O 线程中修改；
2. 跨线程发送时，消息和连接对象在异步执行前仍然有效；
3. 关闭时及时向对端发送 FIN；
4. 关闭后的 Channel 不再产生新的读写回调；
5. 明确当前实现与完整优雅半关闭状态机的差异。

# Action — 设计与实现

### send 的跨线程分发

```cpp
void TcpConnection::send(const std::string& msg) {
    if (!loop_->is_in_loop_thread()) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self, msg] {
            self->send_in_loop(msg);
        });
        return;
    }

    send_in_loop(msg);
}
```

如果当前已经在 I/O 线程，直接执行；否则把任务投递回所属 `EventLoop`。

这里有两个重要保护：

- 捕获 `self`，保证任务真正执行前连接对象不会析构；
- 按值捕获 `msg`，避免异步执行时引用指向已经销毁的临时对象。

因此 socket 写入、写缓冲追加和可写事件修改都在单一线程中串行完成，不需要在 `TcpConnection` 内部加锁。

### close_connection 的关闭路径

```cpp
void TcpConnection::close_connection(Channel& channel) {
    if (isClosed_) {
        return;
    }

    isClosed_ = true;
    connSocket_.shutdown_write();
    channel.disable_all();
    handle_close_callback();
}
```

`shutdown_write()` 对应 `shutdown(fd, SHUT_WR)`，只关闭本端写方向并向对端发送 FIN。它和 C++ 对象析构解耦：即使外部暂时还持有 `shared_ptr`，对端也能及时收到关闭信号。

`disable_all()` 负责停止该 Channel 的后续事件监听，避免关闭回调移除连接后仍有事件访问已经失效的对象。

### 当前实现的边界

当前 `close_connection()` 是“立即结束连接”，不是完整的优雅半关闭状态机：

- 小响应通常已经进入内核发送缓冲区，关闭后可以正常到达；
- 大响应如果仍停留在应用层 `writeBuffer_`，`disable_all()` 会阻止后续可写事件，剩余数据可能被丢弃。

完整的优雅关闭需要额外的 `Disconnecting` 状态：先刷空写缓冲，再 `shutdown_write()`，最后等待对端 EOF 后回收连接。

# Result — 结果

- `send()` 的所有实际写操作都回到所属 I/O 线程；
- 异步发送通过 `shared_ptr` 和值捕获保护连接与数据；
- `shutdown_write()` 让对端及时收到 FIN；
- `disable_all()` 防止关闭后的事件继续访问连接；
- 当前实现简单可靠，但大响应仍需要优雅半关闭状态机才能彻底解决截断风险。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 线程归属 | socket、写缓冲和 Channel 操作统一在 I/O 线程执行。 |
| 异步保活 | Lambda 捕获 `shared_ptr`，保证连接在任务执行前存活。 |
| 数据安全 | 消息按值捕获，避免异步引用悬空。 |
| 关闭语义 | `shutdown_write()` 发送 FIN，`disable_all()` 停止后续事件。 |
| 当前边界 | 没有完整半关闭状态机，大响应可能在写缓冲未清空时被截断。 |

# 面试核心问答总结

## Q1：为什么 send 不能直接在任意线程执行？

socket 写入、写缓冲和 epoll 事件修改不是线程安全的。统一切回 I/O 线程可以无锁串行完成这些操作。

## Q2：为什么异步 Lambda 要捕获 shared_ptr 和消息副本？

`shared_ptr` 保证连接对象仍然存在，消息副本保证异步执行时数据仍然有效。

## Q3：shutdown_write 和 close 有什么区别？

`shutdown_write()` 只关闭发送方向并发送 FIN，C++ 对象和 fd 仍可由后续流程回收；`close()` 会直接释放 fd。当前实现用前者先通知对端，再由生命周期流程完成最终回收。

## Q4：当前关闭实现的主要局限是什么？

它没有等待写缓冲刷空。大响应尚未全部写入内核时立即关闭，可能丢失应用层写缓冲中的剩余数据。
