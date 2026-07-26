# Acceptor fd 耗尽恢复：idle fd 技巧与 busy-loop 防护

`Acceptor` 负责从监听 socket 接收新连接。本文说明进程遇到 `EMFILE` / `ENFILE` 时，为什么会出现 busy-loop，以及如何使用 idle fd 解除这一状态。

# Situation — 情境

正常情况下，`accept4()` 从内核 accept 队列取出一个连接，队列变短，`epoll_wait` 会等待下一批连接。

当进程文件描述符耗尽时，`accept4()` 返回 `EMFILE`，但连接仍留在 accept 队列中。监听 fd 继续保持可读，事件循环会反复被唤醒：

```text
epoll_wait 返回 listen fd 可读
  → accept4() 返回 EMFILE
  → 连接仍在 accept 队列
  → epoll_wait 立即再次返回
  → 重复失败，CPU 空转
```

# Task — 任务

需要在不影响普通瞬态错误处理的前提下：

1. 打破 `EMFILE` / `ENFILE` 导致的 busy-loop；
2. 取走至少一个挂起连接，让监听 fd 不再持续触发；
3. 恢复后继续接受新连接；
4. 不增加常驻线程或复杂同步机制。

# Action — 方案与实现

### idle fd 技巧

Acceptor 启动时预留一个不使用的 fd，例如 `/dev/null`：

```cpp
idleFd_ = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
```

发生 `EMFILE` / `ENFILE` 时执行以下步骤：

1. 关闭 `idleFd_`，腾出一个 fd 名额；
2. 重试 `accept4()`，取走 accept 队列中的一个连接；
3. 不把这个连接交给业务，而是直接将其 fd 作为新的 `idleFd_`；
4. 连接通常会被关闭，对端可能收到连接重置。

关键点是“关闭一个，再接管一个”，fd 占用数基本保持不变，同时成功消耗一个挂起连接。

### 关键代码

```cpp
void Acceptor::on_read(Channel& channel) {
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() < 0) {
        if (errno == EMFILE || errno == ENFILE) {
            accept_idle_connection();
        }
        return;
    }

    // 正常连接交给 TcpServer 处理
    handle_new_connection(std::move(connSocket), clientAddr);
}

void Acceptor::accept_idle_connection() {
    idleFd_ = Socket(-1);

    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() >= 0) {
        idleFd_ = std::move(connSocket);
    }
}
```

其他错误（例如 `EINTR`、`ECONNABORTED`）仍沿用普通重试路径，不应误用 idle fd 恢复。

# Result — 结果

- fd 耗尽时不再因为监听队列持续可读而 busy-loop；
- 挂起连接可以被取走，事件循环恢复正常等待；
- 不需要额外线程；
- idle fd 只在异常路径使用，正常情况下没有额外处理成本。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 问题根因 | `accept4()` 失败但连接仍在 accept 队列，监听 fd 持续可读。 |
| idle fd | 预留一个 fd，在耗尽时关闭并腾出名额。 |
| 恢复动作 | 重试 `accept4()` 取走挂起连接，并将其接管为新的 idle fd。 |
| 错误边界 | 只对 `EMFILE` / `ENFILE` 使用该机制，其他错误按普通路径处理。 |

# 面试核心问答总结

## Q1：为什么 fd 耗尽会造成 busy-loop？

因为 `accept4()` 失败后，连接仍然留在 accept 队列，监听 fd 仍然可读，`epoll_wait` 会立即重复返回。

## Q2：idle fd 技巧解决了什么？

它先关闭预留 fd，再成功 `accept4()` 取走一个挂起连接，从而让 accept 队列变短，打破持续唤醒。

## Q3：为什么不直接忽略 `EMFILE`？

只返回会让监听 fd 一直保持可读，事件循环持续失败。必须实际取走一个连接，才能恢复等待状态。
