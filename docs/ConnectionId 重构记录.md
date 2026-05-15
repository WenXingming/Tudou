# ConnectionId 重构记录

## Situation

当前 `TcpServer` 对外回调直接暴露 `std::shared_ptr<TcpConnection>`。业务层如果长期保存连接对象，就可能在 `EventLoop` 退出或销毁后继续调用 `TcpConnection::send()`，从而触碰已经失效的 `EventLoop*`。

## Task

收回连接生命周期判断权：外部不要把 `TcpConnection` 本体当作稳定身份，而是通过 `ConnectionId` 标识连接，再由 `TcpServer::send(ConnectionId, data)` 统一判断连接是否仍然有效。

## Action

第一阶段先引入 `ConnectionId` 和内部 `ConnectionRecord`。`ConnectionRecord` 由现有 `ConnectionEntry` 演进而来，仍然是 `TcpServer` 私有结构，只是从“保存连接和心跳对象”扩展为“保存连接 id、fd、连接对象、心跳对象和服务器视角下的连接状态”。

不直接使用 fd 作为 `ConnectionId`，因为 fd 是操作系统资源编号，连接关闭后可能很快被复用。旧连接保存的 fd 如果刚好命中新连接，会导致数据误发到另一条连接。`ConnectionId` 由 `TcpServer` 自增生成，作为对外稳定身份；fd 只保留作日志和底层事件定位。

本次重构同时完成了两件事：

- `TcpServer` 的公共回调签名从 `std::shared_ptr<TcpConnection>` 切换为 `ConnectionId` 或 `ConnectionId + payload`
- `HttpServer` 的连接状态表从按 fd 管理迁移为按 `ConnectionId` 管理，响应发送改为统一走 `TcpServer::send(ConnectionId, data)`

## Result

当前结果不能简单写成“生命周期问题已完全解决”，更客观的结论是：

- 对外 API 边界明显改善。上层不再直接拿到 `TcpConnection`，大部分误用入口已经被收回到 `TcpServer`
- `ConnectionId` 解决了“fd 可复用，不适合作为稳定身份”的问题
- `HttpServer` 已跟随迁移，说明新接口不只是停留在 TCP 层内部
- 但这次重构主要解决的是“外部长期持有 `TcpConnection`”这个问题，还没有完全消除 shutdown 期间的所有并发生命周期窗口

## 当前评估

### 已经解决的问题

1. 业务层不再通过 `shared_ptr<TcpConnection>` 直接访问底层连接，公共 API 的生命周期边界更清楚。
2. 连接身份从 fd 切换为 `ConnectionId` 后，避免了 fd 复用导致的误发风险。
3. `TcpServer::send(ConnectionId, data)`、`force_close(ConnectionId)` 和 `stop()` 已经形成了一条统一的服务器侧控制入口。
4. `HttpServer` 的状态仓库已经从 fd 迁移到 `ConnectionId`，说明这次重构不是局部命名替换，而是贯穿到了上层协议层。

### 仍然存在的风险

1. `TcpServer::send` 和 `TcpServer::force_close` 仍然会先在锁内取出 `shared_ptr<TcpConnection>`，再在锁外调用 `conn->send()` / `conn->force_close()`。
2. 如果这两个入口与 `stop()` / `shutdown_connections()` 并发交错，服务器线程可能已经让连接表清空并销毁 `EventLoopThreadPool`，而调用线程还持有局部 `shared_ptr<TcpConnection>`。
3. 此时 `TcpConnection` 本体仍然存活，但内部 `EventLoop* loop_` 可能已经失效，`TcpConnection::send()` / `force_close()` 里仍会继续访问该裸指针。

这说明方案 2 已经收回了“外部长期持有连接对象”这个大问题，但还没有把 `TcpServer` 自己内部的 in-flight 调用窗口彻底封住。换句话说，新的公共接口比旧接口安全得多，但尚未达到“shutdown 并发下绝对不会碰到悬空 loop 指针”的强保证。

## 验证记录

已直接运行与本次重构相关的单元测试二进制：

```bash
cd build && ./test/unitTest/TudouUnitTest --gtest_filter='TcpServerTest.*:HttpServerTest.*'
```

当前已通过的覆盖点包括：

- `TcpServerTest.StopExitsStartFromConnectionCallback`
- `TcpServerTest.InvalidConnectionIdOperationsReturnFalseWhileRunning`
- `TcpServerTest.SendWritesDataToClient`
- `TcpServerTest.ClosingConnectionRejectsSend`
- `HttpServerTest.OnConnectCreatesAndOnCloseRemovesConnectionState`
- `HttpServerTest.ProcessPlainHttpRequestDispatchesRegisteredRouteAndSendsResponse`
- `HttpServerTest.ProcessBadRequestSendsBadRequestAndResetsContext`

这些测试说明：

- `ConnectionId` 版本的公共 API 能正常工作
- `HttpServer` 已成功切换到新接口
- 关闭中连接会拒绝新的 `send`
- `stop()` 能退出 `start()` 主循环

但当前测试仍以单线程 loop 路径为主，尚未专门覆盖“`send/force_close` 与 `stop/shutdown` 在多 IO 线程下并发交错”的那类生命周期竞态。

## 后续建议

如果继续沿方案 2 走，下一步最值得补的是服务器内部的调用期护栏，而不是再把 `TcpConnection` 暴露回去。可以考虑：

1. 为 `send/force_close` 增加更强的关闭同步语义，避免在锁外继续使用可能晚于 loop 生命周期的 `TcpConnection`
2. 增加多 IO 线程下的并发关闭测试，专门覆盖 `stop()`、`shutdown_connections()` 与 `send/force_close()` 交错时序
3. 如果后续还要给上层提供更多连接元数据访问能力，优先通过 `ConnectionId` 查询接口补齐，而不是重新把 `TcpConnection` 暴露给业务层
