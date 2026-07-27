# ConnectionId 重构记录

本次重构的目标是：业务层不再长期持有 `TcpConnection`，而是使用稳定的 `ConnectionId` 作为连接身份，由 `TcpServer` 统一完成发送、关闭和有效性判断。

# Situation — 情境

旧接口直接把 `std::shared_ptr<TcpConnection>` 暴露给外部回调。业务如果长期保存这个对象，可能在 `EventLoop` 退出后继续调用 `send()`，而 `TcpConnection` 内部的 `EventLoop*` 已经失效。

此外，直接使用 fd 作为连接身份也不安全：连接关闭后，操作系统可能把同一个 fd 重新分配给另一条连接。

# Task — 任务

需要完成两件事：

1. 用不会因 fd 复用而混淆的 `ConnectionId` 表示连接；
2. 将发送、关闭和状态查询统一收口到 `TcpServer`。

# Action — 重构方案

### ConnectionId 与 ConnectionRecord

`ConnectionId` 由 `TcpServer` 自增生成，作为对外稳定身份。fd 只用于底层事件定位和日志，不再作为业务层连接标识。

内部的 `ConnectionRecord` 保存连接 ID、fd、连接对象、心跳对象和服务器侧状态，仍由 `TcpServer` 私有管理。

### 公共接口迁移

公共回调从直接传递连接对象改为传递 `ConnectionId` 或 `ConnectionId + payload`。发送统一通过服务器入口完成：

```text
业务持有 ConnectionId
  → TcpServer::send(ConnectionId, data)
  → 查找当前连接
  → 切回所属 EventLoop
  → TcpConnection 发送数据
```

`HttpServer` 的连接状态表也从 fd 迁移为 `ConnectionId`，避免上层再次依赖可复用的 fd。

# Result — 重构结果

- 业务层不再直接持有 `TcpConnection`；
- `ConnectionId` 解决了 fd 复用导致的误发问题；
- `send(ConnectionId, data)`、`force_close(ConnectionId)` 和 `stop()` 形成统一入口；
- `HttpServer` 已完成按 ConnectionId 管理状态；
- 公共 API 的生命周期边界比旧接口更清晰。

# 当前评估

这次重构主要解决“外部长期持有 `TcpConnection`”的问题，并不代表 shutdown 并发窗口已经完全消失。

当前仍需注意：

1. `TcpServer::send()` 和 `force_close()` 可能先取出局部 `shared_ptr`，再在锁外调用连接方法；
2. 如果这时 `stop()` 同时销毁 `EventLoopThreadPool`，连接对象可能还活着，但内部 `EventLoop*` 已失效；
3. 因此，公共接口更安全，但还不能承诺 shutdown 并发下绝对没有 in-flight 调用风险。

# 验证记录

已通过与本次重构相关的测试：

- `TcpServerTest.StopExitsStartFromConnectionCallback`
- `TcpServerTest.InvalidConnectionIdOperationsReturnFalseWhileRunning`
- `TcpServerTest.SendWritesDataToClient`
- `TcpServerTest.ClosingConnectionRejectsSend`
- `HttpServerTest.OnConnectCreatesAndOnCloseRemovesConnectionState`
- `HttpServerTest.ProcessPlainHttpRequestDispatchesRegisteredRouteAndSendsResponse`
- `HttpServerTest.ProcessBadRequestSendsBadRequestAndResetsContext`

这些测试覆盖了 ConnectionId API、HttpServer 迁移、关闭中拒绝发送和 `stop()` 退出主循环等路径。

# 后续建议

下一步应补充多 IO 线程下 `send/force_close` 与 `stop/shutdown_connections` 并发交错的测试，并为服务器内部调用增加更明确的关闭同步语义。

# 面试核心问答总结

## Q1：为什么不能直接用 fd 作为连接 ID？

fd 会在连接关闭后被操作系统复用。旧连接保存的 fd 可能命中新连接，造成数据误发。

## Q2：ConnectionId 解决了什么问题？

它把连接身份与底层 fd 解耦，并让发送和关闭操作统一经过 `TcpServer` 的有效性检查。

## Q3：这次重构是否彻底解决了生命周期问题？

没有。它解决了业务长期持有 `TcpConnection` 的主要风险，但 shutdown 与 in-flight 调用并发交错时，内部仍需要额外同步。
