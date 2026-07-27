# ConnectionHeartbeat：三层防御体系与失活连接心跳检测清理

连接探活不能只依赖 TCP Keep-alive。Tudou 用内核 Keep-alive 发现链路问题，用 `ConnectionHeartbeat` 检测用户态长期空闲，并将最终关闭统一交给 `TcpConnection`。

# Situation — 情境

连接可能因为拔网线、路由故障、对端崩溃或应用假死而长期占用 fd 与内存。TCP Keep-alive 能判断对端内核能否回复 ACK，却无法证明对端应用仍在处理业务。

例如对端进程死锁但操作系统正常时，TCP 探测仍可能成功；对长连接服务而言，服务器仍需要根据用户态数据活动清理长期空闲连接。

# Task — 任务

探活需要：

1. 覆盖链路/对端系统失效和用户态长期无数据两类问题；
2. 不把协议特定的 Ping/Pong 强塞进 TCP 层；
3. 空闲检测在 owner EventLoop 内执行，不引入跨线程连接竞争；
4. 超时后进入统一关闭流程，释放连接与 fd；
5. 不让心跳对象反向长期拥有 TcpConnection。

# Action — 设计与实现

## 三层防御边界

| 层级 | 机制 | 识别的问题 | 当前状态 |
| :--- | :--- | :--- | :--- |
| L4 | TCP Keep-alive | 链路断开、对端系统不可达 | 已由 Socket 配置 |
| L4/L7 | `ConnectionHeartbeat` 空闲检查 | 用户态长期没有入站数据 | 网络库层通用能力 |
| L7 | 协议 Ping/Pong | 协议处理路径与业务响应 | 留给 HTTP/RPC/WebSocket 上层 |

TcpServer 创建连接时设置 `TCP_NODELAY` 和 TCP Keep-alive；Keep-alive 的默认参数为 30 秒空闲后探测、6 秒间隔、连续 3 次失败后判定异常。它是内核层探活，不代表应用还在处理请求。

## 用户态空闲检测流程

成功读取入站数据后刷新活动时间：

```cpp
const ssize_t n = readBuffer_.read_from_fd(connSocket_.fd(), savedErrno);
if (n > 0) {
    if (heartbeat_) {
        heartbeat_->refresh();
    }
    handle_message_callback();
}
```

心跳定时器周期检查 `now - lastActiveTime_`；超时后请求在 owner loop 中关闭连接：

```cpp
auto conn = conn_.lock();
if (!conn) {
    stop();
    return;
}
if (is_timeout(std::chrono::steady_clock::now())) {
    conn->force_close();
}
```

预期关闭路径为：

```text
空闲超时
  → force_close()
  → TcpConnection::close_connection()
  → 停止心跳、shutdown_write、禁用 Channel 事件
  → TcpServer 从 owner loop 连接表移除 owning shared_ptr
  → Socket RAII 关闭 fd
```

# Result — 结果

- 探活责任按内核、网络库、业务协议分层，避免单一机制承担所有语义；
- 用户态空闲检测的刷新点、超时判断和关闭路径已明确；
- 心跳对象只弱引用连接，不会反向延长 TcpConnection 生命周期。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| Keep-alive 边界 | 证明 TCP 对端内核/链路，不证明应用业务健康。 |
| 空闲定义 | 当前以成功收到入站字节刷新，而不是仅看 TCP ACK。 |
| 关闭收敛 | 超时进入 TcpConnection 的统一关闭路径。 |
| 业务扩展 | Ping/Pong 是协议语义，留给上层实现。 |
| 生命周期 | 心跳只弱引用连接，连接回收不依赖心跳对象长期保活。 |

# 面试核心问答总结

## Q1：为什么 TCP Keep-alive 还不够？

它只能说明对端内核仍能回复 TCP 探测，不能说明对端应用仍在读写和处理业务。用户态空闲检测补充了这一盲区。

## Q2：ConnectionHeartbeat 以什么作为活动依据？

当前设计在 `TcpConnection::on_read()` 成功读到数据时刷新活动时间，检测的是用户态是否持续收到入站字节。

## Q3：Ping/Pong 为什么不放进 TCP 层？

Ping/Pong 的报文格式、超时语义和恢复策略依赖 HTTP、RPC、WebSocket 等具体协议。TCP 层只提供通用的连接空闲能力。

## Q4：为什么心跳对象只保存 TcpConnection 的 weak_ptr？

心跳只需要在检查时临时确认连接是否还活着，不应该反向长期拥有连接。连接销毁后弱引用失效，定时检查可以停止或直接返回。
