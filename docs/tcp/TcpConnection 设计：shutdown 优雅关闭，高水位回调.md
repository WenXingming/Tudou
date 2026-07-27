# TcpConnection 设计：shutdown 优雅关闭，高水位回调

`TcpConnection` 把一条连接的收发和关闭收敛到 owner EventLoop。业务线程可以调用 `send()` 或 `force_close()`，但真正修改 Buffer、Channel 和 Socket 的操作始终回到连接所属线程。

# Situation — 情境

业务回调可能运行在非 IO 线程，并在完成后发送响应；连接也可能同时因 EOF、错误、心跳超时或服务器停止进入关闭。若任意线程直接修改 `writeBuffer_` 或 Channel 的事件兴趣，会破坏 One Loop Per Thread 的线程归属。

此外，C++ 对象的析构、TCP 半关闭和 fd 关闭不是同一个动作：必须明确先后关系，避免还有未处理回调时过早关闭资源。

# Task — 任务

发送与关闭路径需要：

1. 跨线程调用安全，不让多个线程直接修改连接状态；
2. 发送尽量直接写入，积压时保留正确的字节顺序；
3. 保留 `writev`，在已有积压时合并旧数据和新消息；
4. 未完成发送依赖 `EPOLLOUT` 续写，不忙等；
5. 写缓冲积压过多时向业务层发出高水位反压信号；
6. 所有关闭原因汇聚到一条可重入、安全的关闭流程。

# Action — 设计与实现

## send：先切换到 owner loop

非 owner 线程调用 `send()` 时，先用 `shared_from_this()` 保活连接，再投递到 owner loop：

```cpp
void TcpConnection::send(std::string msg) {
    if (!loop_->is_in_loop_thread()) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self, msg = std::move(msg)] {
            self->send_in_loop(msg);
        });
        return;
    }
    send_in_loop(msg);
}
```

这样 `writeBuffer_`、`channel_.enable_writing()` 与 Socket 写入都在同一 IO 线程串行执行；lambda 捕获 `self` 防止任务尚未执行时连接被析构。

## send_in_loop：保持发送状态机

输出缓冲为空且未监听写事件时，先直接 `write` 新消息；已有积压时使用 `writev` 发送“旧缓冲 + 新消息”。`writev` 如何利用 Buffer 的可读区见 [Buffer 设计](<Buffer 设计：LT、readv、writev 与收发缓冲.md>)；TcpConnection 关心的是发送状态：未写出的字节进入 `writeBuffer_`，并开启写事件。

```cpp
if (writtenLen < msg.size()) {
    writeBuffer_.write_to_buffer(msg.data() + writtenLen,
                                 msg.size() - writtenLen);
}
channel_.enable_writing();
```

`on_write()` 收到 `EPOLLOUT` 后继续刷出 writeBuffer_；仅当缓冲完全清空时关闭写事件并触发写完成回调。

## 高水位：通知业务处理慢客户端

非阻塞发送不会卡住 EventLoop，但慢客户端会让未发送字节持续积压在 `writeBuffer_`。TcpConnection 不擅自丢弃数据或停止业务，而是在缓冲首次跨越阈值时通知业务层：

```cpp
const size_t oldLen = writeBuffer_.readable_bytes();
// 尝试 write / writev，剩余字节写入 writeBuffer_

const size_t newLen = writeBuffer_.readable_bytes();
if (highWaterMarkCallback_
    && oldLen < highWaterMark_
    && newLen >= highWaterMark_) {
    handle_high_water_mark_callback();
}
```

这是边缘触发：缓冲已经高于阈值时，后续 `send()` 不重复通知；只有它被排空到阈值以下、之后再次跨越时才触发。TcpServer 包装该回调并向业务提供当前积压字节数，业务可选择暂停上游生产、限速或关闭异常慢的连接。

写缓冲清空时会触发 `writeCompleteCallback_`，可作为简单恢复信号：

```text
首次跨越高水位 → 业务暂停或降速
  → EPOLLOUT 持续排空 writeBuffer_
  → writeCompleteCallback → 业务恢复生产
```

高水位只是反压信号，不是硬性内存上限；若需要强制限制，业务仍需定义拒绝发送、关闭连接或全局额度等策略。

## close：一条可重入的关闭路径

EOF、读写错误、心跳超时和服务器停止最终都调用 `close_connection()`：

```cpp
void TcpConnection::close_connection() {
    if (isClosed_) {
        return;
    }
    isClosed_ = true;
    if (heartbeat_) {
        heartbeat_->stop();
    }
    connSocket_.shutdown_write();
    channel_.disable_all();
    handle_close_callback();
}
```

`isClosed_` 使多种关闭来源可以安全汇合。`shutdown_write()` 关闭发送方向并发送 FIN，表达“不再发送数据”；`disable_all()` 停止普通读写事件，但 Channel 仍会在析构时从 Poller 注销。close callback 由 TcpServer 移除连接表中的 owning `shared_ptr`，对象和 Socket 最终通过 RAII 析构。

这里的“优雅”应准确理解：已写入**内核发送缓冲区**的数据会按 TCP 的 FIN 语义收尾；当前实现不会等待应用层 `writeBuffer_` 排空，调用 `close_connection()` 后其中尚未进入内核的字节不会继续发送。若业务需要“发送完响应后再 FIN”，需要增加独立的 drain-then-shutdown 状态，而不是复用 `force_close()`。

`force_close()` 的语义是“请求 owner loop 尽快走关闭路径”，不是让调用线程直接 `close(fd)`：

```cpp
void TcpConnection::force_close() {
    if (!loop_->is_in_loop_thread()) {
        auto self = shared_from_this();
        loop_->queue_in_loop([self] { self->close_connection(); });
        return;
    }
    close_connection();
}
```

# Result — 结果

- 连接可变状态始终由 owner loop 串行修改；
- 发送优先走直接写，积压时保留 `writev` 的合并发送能力；
- 短写和 `EAGAIN` 转化为 Buffer + `EPOLLOUT` 的正常续写路径；
- 高水位边缘触发让慢客户端积压对业务可见，但不把限流策略硬编码进 TCP 层；
- 多个关闭来源不会重复执行清理；
- TCP 写端半关闭、Channel 注销、连接表移除和 Socket RAII 各自职责明确；
- 当前 close 路径不保证排空应用层 writeBuffer，完整 drain-then-shutdown 属于额外能力。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 线程归属 | `send` / `force_close` 跨线程时投递回 owner loop。 |
| 生命周期 | 投递 lambda 捕获 `shared_ptr`，执行前连接不会析构。 |
| 发送策略 | 无积压时直写；有积压时 `writev`；剩余数据进入 Buffer。 |
| 写事件 | 仅有待发送字节时开启 `EPOLLOUT`，清空后关闭。 |
| 高水位 | `writeBuffer_` 首次跨阈值时通知业务，清空后以写完成回调作为恢复信号。 |
| 关闭收敛 | `isClosed_`、停止心跳、`shutdown_write`、禁用事件、关闭回调。 |
| 关闭边界 | 当前关闭的是写方向，不等待应用层 writeBuffer 排空。 |

# 面试核心问答总结

## Q1：业务线程调用 send 为什么不会产生 Buffer 竞争？

业务线程不直接写 Buffer，而是把任务投递到连接所属 EventLoop。真正的 `send_in_loop` 只在 owner 线程执行。

## Q2：为什么保留 writev？

当 writeBuffer 已有积压时，`writev` 可以把旧缓冲和新消息一次交给内核，保持顺序并避免先在用户态拼接成新连续块。

## Q3：shutdown_write 和 close 有什么区别？

`shutdown_write` 关闭发送方向并发送 FIN，fd 仍由 Socket 对象持有；真正的 fd `close` 发生在 Socket 析构。它只保证已进入内核发送缓冲区的数据按 FIN 语义收尾；当前实现不会等待应用层 writeBuffer 排空。

## Q4：高水位回调解决什么问题？

慢客户端会让未发送字节积压在应用层 writeBuffer。高水位回调只在首次跨过阈值时通知业务，使业务能够暂停、限速或关闭异常连接，避免无限制内存增长。

## Q5：为什么 force_close 也要切回 owner loop？

关闭会修改 Channel 事件、心跳和连接表回调，这些都属于 owner loop。外部线程直接关闭 fd 会打破事件顺序并可能让 epoll 中的 Channel 状态失配。
