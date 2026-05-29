# Acceptor fd 耗尽恢复：idle fd 技巧与 busy-loop 防护

在网络服务器开发中，文件描述符（fd）耗尽（`EMFILE`/`ENFILE`）是一个容易被忽略但后果严重的问题。本文基于 **STAR 法则** 记录 Tudou 网络库 Acceptor 组件中该问题的发现、分析与解决过程。

---

## 1. Situation（情境）

Tudou 的 `Acceptor` 负责监听 listen fd 上的新连接事件，通过 `accept4` 系统调用取出连接并交给上层 `TcpServer` 处理。原始实现中，`accept4` 失败时统一 log + return：

```cpp
void Acceptor::on_read(Channel& channel) {
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() < 0) {
        return;  // 所有错误都静默跳过
    }
    ...
}
```

这种处理在大多数瞬态错误（`ECONNABORTED`、`EPROTO`、`EINTR`）下没有问题——下一次 `epoll_wait` 会重新触发 `on_read`，重试即可。但当进程 fd 耗尽时，会触发一个**CPU busy-loop**。

## 2. Task（任务）

识别 fd 耗尽场景下的 busy-loop 根因，并实现一种零额外开销的恢复机制，确保：

1. fd 耗尽时不会导致 CPU 空转。
2. 恢复后 Acceptor 能正常继续接受新连接。
3. 不影响其他瞬态错误的正常处理路径。

## 3. Action（行动）

### 3.1 根因分析

Linux 内核的 accept 队列工作方式：

```
客户端 A 发 SYN → 三次握手完成 → 放入 accept 队列
客户端 B 发 SYN → 三次握手完成 → 放入 accept 队列
```

**只要 accept 队列非空，listen fd 在 epoll 中就是可读的。**

正常情况下：`on_read` → `accept4` 取出一个连接 → 队列变短 → 下次 `epoll_wait` 才返回。

fd 耗尽时的 busy-loop：

```
accept 队列: [A, B]   ← 非空

epoll_wait 返回（listen fd 可读）
  → on_read
    → accept4() → -1, EMFILE    ← 失败！连接 A 仍在队列中
    → return

epoll_wait 立刻返回（listen fd 仍可读，队列非空）
  → on_read
    → accept4() → -1, EMFILE    ← 又失败
    → return

epoll_wait 立刻返回...    ← 死循环，CPU 100%
```

**根因**：`accept4` 失败意味着连接没有从队列中取出，队列永远非空，`epoll_wait` 永远立刻返回。

### 3.2 解决方案：idle fd 技巧

这是 nginx、leveldb、libevent 等成熟网络库处理 `EMFILE` 的标准模式。

**核心思路**：预先持有一个不使用的空闲 fd，fd 耗尽时关闭它腾出一个名额，重试 `accept4` 拉走挂起连接，然后将 accepted fd 直接作为新的 idle fd。

```cpp
// 构造时通过 open("/dev/null") 预留，只占 1 个 fd，无多余端点
idleFd_ = Socket(::open("/dev/null", O_RDONLY | O_CLOEXEC));
```

恢复流程：

```
fd 表: [0,1,...,1023]  ← 全满，EMFILE。idleFd_ = fd=1000

1. close(idleFd_)             → 腾出 1 个 fd 名额
   fd 表: [0,1,...,1022]（1 个空闲）

2. accept4() → 成功，返回 fd=1000  → 拉走挂起连接，队列变短
   不关闭 accepted fd，直接作为新的 idleFd_
   fd 表: [0,1,...,1023]（又满了，但队列短了 1 个）
```

关键设计：**fd 占用数在整个恢复过程中不变**（关 1 个旧 idle → 开 1 个 accepted → 直接接管为新 idle），因此始终只需要 1 个空闲槽位，不存在分配失败的可能。对端会收到连接重置，但在 fd 耗尽状态下这是不可避免的。

### 3.3 最终实现

```cpp
void Acceptor::on_read(Channel& channel) {
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() < 0) {
        // EMFILE/ENFILE：fd 耗尽，内核队列中的挂起连接无法取出，会导致 epoll 持续触发 busy-loop。
        // 通过关闭预留的 idle fd 腾出名额、重试 accept 拉走挂起连接来打破循环。
        if (errno == EMFILE || errno == ENFILE) {
            accept_idle_connection();
        }
        return;  // 其他瞬态错误：忽略，等下次 epoll 触发
    }
    ...
}

void Acceptor::accept_idle_connection() {
    spdlog::error("Acceptor: fd exhausted (EMFILE/ENFILE), entering recovery");

    // 1. 关闭 idle fd 腾出 1 个 fd 名额。
    idleFd_ = Socket(-1);

    // 2. 重试 accept 拉走内核队列中的挂起连接，直接接管其 fd 作为新的 idle fd。
    //    fd 占用数不变（关 1 个 + 开 1 个），对端会收到连接重置，fd 耗尽状态下不可避免。
    sockaddr_in clientAddr{};
    Socket connSocket = listenSocket_.accept(&clientAddr);
    if (connSocket.fd() >= 0) {
        idleFd_ = std::move(connSocket);
    }
}
```

## 4. Result（结果）

| 指标 | 修复前 | 修复后 |
|------|--------|--------|
| fd 耗尽时 CPU | 100% busy-loop | 立即恢复，零空转 |
| 队列中挂起连接 | 无法取出，持续触发 epoll | 被拉走，队列清空 |
| 恢复后状态 | 无恢复机制 | accepted fd 直接接管为新 idle fd，正常继续监听 |
| 额外系统调用 | 无 | 恢复时仅 1 次 accept4（关旧 idle + 接管新 idle 无额外开销） |

### 参考

- nginx: `src/event/ngx_event_accept.c` 中的 `ngx_close_idle_connections` 机制
- leveldb: `util/env_posix.cc` 中的 `posixConnect` EMFILE 处理
- libevent: `evutil.c` 中的 `evutil_ersocket_emfile` 函数
