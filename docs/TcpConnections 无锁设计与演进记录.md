# TcpConnections 无锁设计与演进记录

本文档分为两部分：首先记录过去在连接管理与发送路由上的设计尝试及遇到的瓶颈；其次利用 STAR 法则详细描述当前 Tudou 中 `TcpConnections` 的无锁设计方案。

## 一、 历史尝试记录：ConnectionId 与双层 Hash

### 1. 引入 ConnectionId 的初衷
在早期的设计中，为了防止业务层长期持有底层 `TcpConnection` 的智能指针导致生命周期问题（例如 EventLoop 销毁后仍尝试访问），Tudou 将 HTTP 响应路径从“消息回调里直接调用 `conn->send()`”改成了“业务层只持有 `ConnectionId`，通过 `TcpServer::send(id, data)` 进行发送”。

### 2. 性能瓶颈与发现
在随后的 benchmark 中发现，这种严格的边界划分带来了显著的性能损耗。每发送一条响应，都需要经历：
1. 进入 `TcpServer::send(id, data)`。
2. 加锁查询全局连接表（或辅助索引），完成 `id -> TcpConnection` 的反向查找。
3. 获取到连接对象后，再调用 `conn->send()`。

相比于 muduo 直接在回调中通过连接对象发送响应，这种设计在热路径上多了一层中转和查找开销。

### 3. 中间尝试：双层 Hash 规避全局锁
为了解决全局锁的竞争，中间尝试过引入 `EventLoop* -> ConnectionId -> ConnectionRecord` 的双层 Hash 表结构。这种方案试图让 owner loop 的热路径绕开全局锁，但只要保留 `TcpServer::send(id, data)` 接口，就不可避免地需要维护一套复杂的辅助索引。它虽然缓解了锁竞争，却将巨大的复杂度留在了 `TcpServer` 内部。

### 4. 最终反思与决策
最终我们意识到：HTTP 层处理的本就是一条物理连接上的协议状态，传给 HTTP 层的概念理应是“连接”本身。试图用 `ConnectionId` 彻底隔离业务层和网络层的代价过高，且偏离了高性能 Reactor 框架的初衷。因此，决定废弃 `ConnectionId` 发送接口，回归直接传递 `TcpConnectionPtr`，并在此基础上重新设计无锁的连接管理机制。

---

## 二、 TcpConnections 无锁设计 (STAR 法则)

### Situation（情境）
放弃 `ConnectionId` 并回归直接传递 `TcpConnectionPtr` 后，虽然解决了热路径的反向查找开销，但我们仍需在 `TcpServer` 中集中管理所有存活连接（用于生命周期兜底、心跳检测和优雅停机）。
在多线程 Reactor 模型中，如果所有 IO 线程在建立新连接、断开连接或频繁刷新心跳时，都去竞争同一把全局连接表锁（例如用 `std::mutex` 保护一个平铺的 `connections_` map），将会产生严重的锁冲突，直接拉低网络库的并发吞吐量。

### Task（任务）
设计一套无锁的连接管理机制，目标如下：
1. **热路径无锁**：HTTP 消息的接收和发送路径中，绝不能发生全局锁竞争。
2. **遵循 One Loop Per Thread**：连接表的数据结构必须契合线程归属原则，做到局部化管理。
3. **安全的生命周期管理**：连接的创建、销毁、心跳更新必须是线程安全的；同时在服务器 `stop()` 触发优雅停机时，能够安全地同步等待所有存活连接清理完毕。

### Action（行动）

**1. 二级无锁连接表设计**
`TcpServer` 将原本的全局平铺连接表改为按 `EventLoop` 分组的二级映射表：
```cpp
using ConnectionRecords = std::unordered_map<TcpConnection*, ConnectionRecord>;
std::unordered_map<EventLoop*, ConnectionRecords> connectionRecordsByLoop_;
```

HTTP 响应路径变成：

```text
TcpConnection::on_read
  -> TcpServer::on_message(conn)
  -> HttpServer::on_message(conn)
  -> conn->receive()
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
using std::unordered_map<EventLoop*, ConnectionRecords> = std::unordered_map<EventLoop*, ConnectionRecords>;

std::unordered_map<EventLoop*, ConnectionRecords> connectionRecordsByLoop_;
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
  -> HttpServer::on_message(conn)
  -> conn->receive()
  -> conn->send(response)
  -> TcpConnection::send_in_loop
  -> 小响应优先直接 write(fd)
```

`TcpServer` 现在只做连接编排和生命周期收口，不再作为响应发送的中转站。最终我们的性能相比于有锁版本提升了约 30%（60w QPS -> 80w+ QPS），已经和 muduo 处于同一数量级：
