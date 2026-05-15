# TcpConnections 无锁设计

本文用 STAR 法则记录这次 TcpConnections 重构：Situation 说明问题，Task 说明目标，Action 说明实现动作，Result 说明结果和验证。

## S - Situation：为什么要做

早期 benchmark 中，Tudou 的 HTTP 响应路径从“消息回调里直接 `conn->send()`”改成了“业务层只持有 `ConnectionId`，通过 `TcpServer::send(id, data)` 发送”。这个边界看起来能避免业务层长期保存 `TcpConnection`，但把每条响应都变成：

1. 进入 `TcpServer::send`
2. 查连接表或 owner loop 索引
3. 找回 `TcpConnection`
4. 再调用 `conn->send`

这和 muduo 的 benchmark 路径不一致。muduo 的回调直接拿连接对象，响应直接从连接发出，没有再经由 server 做一次 id 查找。

中间尝试过 `EventLoop* -> ConnectionId -> ConnectionRecord` 的双层 hash。它能让 owner loop 热路径绕开全局连接锁，但只要保留 `TcpServer::send(id, data)`，就仍然需要一套 `id -> owner loop` 的辅助索引。这个方案解决了锁竞争，却把复杂度留在了 `TcpServer` 内部。

最终判断是：HTTP 层处理的是一条连接上的协议状态，传给 HTTP 层的也应该是连接这一概念。`ConnectionId` 发送 API 不再保留。

## T - Task：要达成什么

目标：

- 响应热路径不再进入 `TcpServer::send(id, data)`。
- 响应热路径不再做 `id -> TcpConnection` 反向查找。
- 连接表只服务生命周期、heartbeat 和 shutdown，不参与发送。
- 每个连接记录只由所属 `EventLoop` 线程读写，符合 OneLoopPerThread 的无锁边界。
- 保留 `TcpConnection::send` / `force_close` 的跨线程安全语义。

非目标：

- 不把 owner loop 编进 `ConnectionId`。
- 不保留 `ConnectionId` 发送兼容接口。
- 不引入额外通用抽象来包装连接表。

## A - Action：怎么做

### 1. 回调直接传 TcpConnectionPtr

`TcpServer` 的上层回调改为直接传递 `TcpConnectionPtr`：

```cpp
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using MessageCallback = std::function<void(const TcpConnectionPtr&, const std::string&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
```

HTTP 响应路径变成：

```text
TcpConnection::on_read
  -> TcpServer::on_message(conn)
  -> HttpServer::on_message(conn, data)
  -> conn->send(response)
  -> TcpConnection::send_in_loop
```

这里没有 `ConnectionId`，没有 `TcpServer::send`，也没有 `id -> TcpConnection` 的反向查找。`TcpConnection::send` 自己就是线程安全入口：同 loop 线程直接执行，跨线程调用时投递回所属 `EventLoop`。

### 2. 小响应直接 write(fd)

`TcpConnection::send_in_loop` 增加快路径：

1. 当前已经在连接所属 loop 线程
2. 连接未关闭
3. 用户态发送缓冲为空
4. Channel 当前没有关注写事件

满足条件时先直接 `write(fd)`。如果一次写完，就异步触发 write-complete 回调并返回；如果只写了一部分，或遇到 `EAGAIN` / `EWOULDBLOCK`，只把剩余数据追加到 `writeBuffer_`，再关注 EPOLLOUT。

这个优化保留原有背压语义：高水位只统计用户态 `writeBuffer_` 的积压，已经写进内核发送缓冲的数据不计入高水位。

### 3. TcpServer 只保存连接记录

`TcpServer` 仍然需要保存连接，但用途已经收缩为生命周期管理、心跳对象管理和 shutdown 收口。

当前结构是：

```cpp
struct ConnectionRecord {
    TcpConnectionPtr connection;
    std::shared_ptr<ConnectionHeartbeat> heartbeat;
};

using ConnectionRecords = std::unordered_map<TcpConnection*, ConnectionRecord>;
using ConnectionRecordsByLoop = std::unordered_map<EventLoop*, ConnectionRecords>;

ConnectionRecordsByLoop connectionRecordsByLoop_;
std::atomic<size_t> activeConnectionCount_;
```

`connectionRecordsByLoop_` 的一级 key 是 owner `EventLoop*`。二级表只在该 owner loop 所属线程读写，因此它不是共享热数据结构，不需要全局连接锁。

`activeConnectionCount_` 只用于 shutdown：主 loop 退出后，`shutdown_connections` 把关闭动作投递给各个 owner loop，然后等待所有连接通过 `remove_connection` 清空。它不参与发送路径。

### 4. 连接生命周期仍由 owner loop 收口

创建连接：

```text
main loop accept
  -> 选择 ioLoop
  -> ioLoop 创建 TcpConnection
  -> connectionRecordsByLoop_[ioLoop][conn.get()] = record
```

消息到达：

```text
owner loop
  -> TcpServer::on_message(conn)
  -> refresh_connection_heartbeat(conn)
  -> messageCallback_(conn, data)
```

关闭连接：

```text
TcpConnection close callback
  -> TcpServer::remove_connection(conn)
  -> owner loop 删除本地 connection record
  -> stop heartbeat
```

shutdown：

```text
TcpServer::shutdown_connections
  -> 遍历 connectionRecordsByLoop_
  -> 对每个 owner loop 投递 force_close
  -> 等待 activeConnectionCount_ 归零
```

关闭任务中的局部 `TcpConnectionPtr` 在 owner loop 内释放，避免最后一个 `TcpConnection` 引用落在主线程析构，保证 `Channel` 仍在 owner loop 线程销毁。

## R - Result：结果是什么

这次重构后，默认 HTTP benchmark 的关键响应路径已经去掉：

- 每条响应发送时的 `TcpServer::send(id, response)` 查表。
- 为支持 `ConnectionId` 发送而引入的 owner loop 辅助索引。
- 每条消息刷新 heartbeat 时的全局连接表锁。

当前响应路径更接近 muduo：

```text
on_message(owner loop)
  -> HttpServer::on_message(conn, data)
  -> conn->send(response)
  -> TcpConnection::send_in_loop
  -> 小响应优先直接 write(fd)
```

`TcpServer` 现在只做连接编排和生命周期收口，不再作为响应发送的中转站。

## 验证

本次重构后已通过：

```bash
cmake --build build
./build/test/unitTest/TudouUnitTest
```

覆盖点：

- `TcpServerTest.SendFromMessageCallbackWorksInMultiThreadMode`
- `HttpServerTest.ProcessPlainHttpRequestDispatchesRegisteredRouteAndSendsResponse`
