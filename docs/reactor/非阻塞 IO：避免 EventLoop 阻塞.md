# 非阻塞 I/O：避免 EventLoop 阻塞

Tudou 将监听 socket 和连接 socket 设为非阻塞，并用 `epoll` 等待状态变化。非阻塞 I/O 保证一次 `accept`、`read` 或 `write` 暂时无法完成时，不会卡住整个 EventLoop。

# Situation — 情境

epoll 通知 fd 可读或可写，只表示“此刻可能进行 I/O”。在真正调用 `read`、`write`、`accept` 时，数据可能已被消费、内核缓冲区也可能再次满或空。

若 fd 是阻塞模式，一个慢客户端就能让当前 IO 线程睡眠，进而让同一 EventLoop 负责的所有连接都无法继续处理。高并发 Reactor 必须避免这种阻塞。

# Task — 任务

I/O 层需要保证：

1. 监听与连接 socket 都不会阻塞 EventLoop；
2. `EAGAIN`、`EWOULDBLOCK`、短读、短写被视为正常状态；
3. 未写完数据在用户态保存，并在下次可写时续传；
4. EOF 和真正错误走统一关闭或错误处理；
5. 区分阻塞/非阻塞、同步/异步、I/O 多路复用，避免概念混淆。

# Action — 设计与实现

## 创建阶段即设为非阻塞

监听 socket 在创建时携带 `SOCK_NONBLOCK`，连接 socket 则由 `accept4` 一次性设置非阻塞和 close-on-exec：

```cpp
const int listenFd = ::socket(AF_INET,
    SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_TCP);

const int connFd = ::accept4(fd(),
    reinterpret_cast<sockaddr*>(&peerAddr), &addrLen,
    SOCK_NONBLOCK | SOCK_CLOEXEC);
```

`accept4` 避免了“先 accept、再 fcntl”的额外系统调用和短暂竞态窗口。高并发下瞬时断开也可能让它返回 `EAGAIN`、`EWOULDBLOCK`、`EINTR` 或 `ECONNABORTED`；这些不是服务器应立即退出的致命错误。

## 读取：把 EAGAIN 当作正常控制流

连接可读时，`TcpConnection::on_read()` 从 fd 写入读缓冲区：

```cpp
const ssize_t n = readBuffer_.read_from_fd(connSocket_.fd(), savedErrno);
if (n > 0) {
    handle_message_callback();
    return;
}
if (n == 0) {
    close_connection();      // 对端发送 EOF
    return;
}
if (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK) {
    return;                  // 暂时无数据，回到 EventLoop
}
handle_error_callback();
close_connection();
```

可读事件与真正 `read` 之间的状态可能变化，因此即使已收到 epoll 通知，也必须处理 `EAGAIN`。

## 写入：直接写、缓冲剩余数据、等待下一次可写

发送时，如果输出缓冲区为空，先尝试直接 `write`；有积压数据时使用 `writev` 合并“旧缓冲 + 新消息”，减少一次用户态拼接。没有写完的字节放入 `writeBuffer_`，并开启 `EPOLLOUT`：

```cpp
if (writtenLen < msg.size()) {
    writeBuffer_.write_to_buffer(msg.data() + writtenLen,
                                 msg.size() - writtenLen);
}
channel_.enable_writing();
```

收到可写事件后继续发送；仅当输出缓冲区完全清空时才关闭写事件：

```cpp
const ssize_t n = writeBuffer_.write_to_fd(connSocket_.fd(), savedErrno);
if (n < 0 && (savedErrno == EAGAIN || savedErrno == EWOULDBLOCK)) {
    return;
}
if (writeBuffer_.readable_bytes() > 0) {
    return;
}
channel_.disable_writing();
handle_write_complete_callback();
```

持续关注 `EPOLLOUT` 会让 socket 几乎一直处于“可写”状态，造成无意义唤醒；所以只有存在待发送数据时才开启写事件。

## 三个容易混淆的概念

| 概念 | 含义 | Tudou 中的角色 |
| :--- | :--- | :--- |
| 阻塞 / 非阻塞 | I/O 条件不满足时，调用线程是否睡眠 | socket 使用非阻塞模式 |
| 同步 / 异步 | I/O 操作由谁完成、结果何时交付 | 应用主动调用 `read` / `write`，属于同步 I/O |
| I/O 多路复用 | 同时监视多个 fd 是否就绪 | epoll 负责等待就绪状态 |

因此 Tudou 的模型是：**同步 I/O + 非阻塞 fd + epoll 多路复用**。epoll 不是异步 I/O；它不读取数据，也不保证一次写完。

当前 Channel 的事件掩码没有设置 `EPOLLET`，因此使用 epoll 默认的水平触发语义。只要仍满足就绪条件，后续 `epoll_wait` 仍会通知；即便如此，非阻塞和完整返回值处理仍不可省略。

# Result — 结果

- 慢连接或暂时不可读写的连接不会阻塞整个 EventLoop；
- `EAGAIN`、短读和短写进入可预测的缓冲与重试路径；
- 只在有积压数据时监听可写事件，避免无效唤醒；
- EOF、系统错误与正常暂不可用状态被明确区分；
- `writev` 在有发送积压时合并输出，减少额外用户态拷贝。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 非阻塞创建 | `socket` / `accept4` 直接设置 `SOCK_NONBLOCK`。 |
| 读取状态机 | `> 0` 处理消息，`0` 关闭，`EAGAIN` 返回，其他错误关闭。 |
| 写入状态机 | 先直写或 `writev`，剩余数据入 Buffer，`EPOLLOUT` 续写。 |
| 写事件开关 | 仅在输出缓冲非空时开启，写完立即关闭。 |
| I/O 模型 | 同步 I/O、非阻塞 fd、epoll 多路复用。 |

# 面试核心问答总结

## Q1：epoll 已经通知可读，为什么还必须使用非阻塞 socket？

就绪通知与实际 read 之间状态可能变化，或一次读只能得到部分数据。非阻塞保证即使暂时无数据，IO 线程也会得到 `EAGAIN` 并继续服务其他连接，而不是睡眠。

## Q2：遇到 EAGAIN 应该关闭连接吗？

不应该。它表示当前操作暂时不能继续，不是连接异常。读路径直接返回；写路径保留未发送数据并等待下一次可写事件。

## Q3：为什么发送不完时要开启写事件？

内核发送缓冲区满时，应用不能忙等重试。把剩余字节留在 `writeBuffer_` 并监听 `EPOLLOUT`，内核可继续接收数据时才唤醒 EventLoop 续写。

## Q4：epoll 是异步 I/O 吗？

不是。Tudou 仍由应用线程调用 read/write 并处理结果，因此是同步 I/O；epoll 只是多路复用机制，用来等待多个 fd 的就绪状态。
