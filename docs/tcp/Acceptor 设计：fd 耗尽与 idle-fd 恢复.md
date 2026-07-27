# Acceptor 设计：fd 耗尽与 idle-fd 恢复

`Acceptor` 负责监听 socket 的可读事件并接收新连接。进程达到文件描述符上限时，它用预留的 idle fd 打破 accept 队列持续可读导致的 busy-loop。

# Situation — 情境

监听 fd 可读表示 accept 队列中存在待接收连接。若进程已经无法再打开 fd，`accept4()` 返回 `EMFILE`；连接仍留在队列中，因此监听 fd 仍保持可读。

```text
epoll_wait 返回 listen fd 可读
  → accept4() 返回 EMFILE
  → 连接仍留在 accept 队列
  → epoll_wait 立即再次返回
  → 持续失败，CPU 空转
```

这不是普通的“accept 失败后返回即可”：只返回不会消耗队列中的连接，EventLoop 会反复收到同一个事件。

# Task — 任务

异常恢复路径需要：

1. 只处理 `EMFILE` / `ENFILE`，不混淆普通瞬态错误；
2. 至少取走一个挂起连接，解除监听 fd 的持续可读状态；
3. 保持进程的 fd 占用稳定，并恢复预留 fd；
4. 不增加常驻线程或复杂同步。

# Action — 设计与实现

## 正常 accept 路径

监听 socket 和已连接 socket 都通过 `SOCK_NONBLOCK | SOCK_CLOEXEC` 创建。listen fd 可读时，Acceptor 调用 `accept4()`，成功的 `Socket` 被交给 TcpServer 分配到 IO loop。

```cpp
Socket connSocket = listenSocket_.accept(clientAddr);
if (connSocket.fd() < 0) {
    if (errno == EMFILE || errno == ENFILE) {
        accept_idle_connection();
    }
    return;
}
handle_connect_callback(std::move(connSocket), peerAddr);
```

`EAGAIN`、`EWOULDBLOCK`、`EINTR`、`ECONNABORTED` 等错误不进入 idle-fd 路径；它们不表示 fd 配额已耗尽。

## idle fd 恢复路径

Acceptor 启动时打开 `/dev/null`，保留一个不参与业务的 fd。发生耗尽时，当前代码执行：

```cpp
void Acceptor::accept_idle_connection() {
    idleFd_ = Socket(-1);                 // 1. 释放一个 fd 名额

    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(clientAddr);
                                       // 2. 接走一个挂起连接

    idleFd_ = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
                                       // 3. 重新建立预留 fd
}                                      // 4. connSocket 析构，关闭刚接走的连接
```

取走的连接不会进入业务层；函数结束时 `connSocket` 的 RAII 析构关闭该 fd。它的目的不是服务这个客户端，而是让 accept 队列减少一个条目，使监听 fd 不再因同一连接持续触发。

## 设计边界

这只能保护进程自身 fd 耗尽造成的 EventLoop 忙转，不能解决根因。发生 `EMFILE` 时仍应检查连接泄漏、日志 fd、文件打开策略和进程的 `RLIMIT_NOFILE` 配置。

# Result — 结果

- `EMFILE` / `ENFILE` 时不再反复处理同一个监听事件；
- 异常路径只关闭并重新打开一个预留 fd，正常 accept 路径无额外成本；
- 一个挂起连接被明确丢弃，EventLoop 能恢复等待；
- Socket RAII 保证恢复路径中的临时连接不会泄漏 fd。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 根因 | accept 失败不等于队列变空，listen fd 仍会保持可读。 |
| 预留 fd | `/dev/null` 提供一个可临时释放的 fd 名额。 |
| 恢复流程 | 释放 idle fd → accept 一个连接 → 丢弃连接 → 重建 idle fd。 |
| 适用范围 | 仅处理 `EMFILE` / `ENFILE`，不掩盖其他 accept 错误。 |
| 根因治理 | 仍需排查 fd 泄漏和系统资源上限。 |

# 面试核心问答总结

## Q1：为什么 fd 耗尽会造成 busy-loop？

因为 accept 失败后连接还在内核 accept 队列中，监听 fd 仍然可读。epoll 会立即再次返回这个 fd，EventLoop 不断失败重试。

## Q2：idle fd 技巧具体做了什么？

先关闭预留 `/dev/null` fd 腾出一个名额，再 accept 并立即丢弃一个挂起连接，最后重新打开 `/dev/null`。这样消耗了队列中的一个连接，打破持续可读状态。

## Q3：为什么不把恢复时 accept 到的连接交给业务？

进程刚刚证明自身 fd 已耗尽，没有资源安全地维护新连接。恢复路径的目标是保护 EventLoop，而不是在资源枯竭时继续扩大负载。
