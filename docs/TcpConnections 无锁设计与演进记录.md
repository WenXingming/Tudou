# TcpConnections 无锁设计与演进记录

本文记录连接管理从 `ConnectionId` 方案回退到直接传递 `TcpConnectionPtr` 的原因，以及当前按 owner `EventLoop` 分组的无锁连接表设计。

# Situation — 历史尝试与问题

早期方案让业务只持有 `ConnectionId`，通过 `TcpServer::send(id, data)` 发送，以避免外部长期持有 `TcpConnection`。

但这条路径的每次发送都需要：

```text
业务响应
  → TcpServer::send(id, data)
  → 加锁查找 id 对应的连接
  → 调用 TcpConnection::send()
```

它在热路径上增加了锁和反向查找。为消除全局锁而尝试的双层 Hash 又把复杂度集中到了 `TcpServer`，收益有限。

最终决定让 HTTP 层直接持有当前请求对应的 `TcpConnectionPtr`，把无锁重点放在服务器连接表，而不是在每次响应发送时绕路。

# Task — 任务

新的连接管理方案需要：

1. 消除响应发送路径上的全局连接表查找；
2. 遵循 One-Loop-Per-Thread，让连接记录由 owner loop 管理；
3. 支持心跳、生命周期和 shutdown 收口；
4. 小响应尽可能直接写入 socket；
5. 保证最后一个连接引用在 owner loop 中释放。

# Action — 当前设计

### 按 owner loop 分组的连接表

```cpp
struct ConnectionRecord {
    TcpConnectionPtr connection;
    std::shared_ptr<ConnectionHeartbeat> heartbeat;
};

using ConnectionRecords =
    std::unordered_map<TcpConnection*, ConnectionRecord>;

std::unordered_map<EventLoop*, ConnectionRecords>
    connectionRecordsByLoop_;
std::atomic<size_t> activeConnectionCount_;
```

一级 key 是 owner `EventLoop*`，二级表只由对应 loop 线程读写，因此连接记录不需要一把覆盖所有连接的全局锁。

### 响应发送路径

```text
owner loop
  → TcpServer::on_message(conn)
  → HttpServer::on_message(conn)
  → conn->receive()
  → conn->send(response)
  → TcpConnection::send_in_loop
```

`TcpConnection::send()` 已经是线程安全入口：同 loop 线程直接执行，其他线程投递回 owner loop。响应发送不再经过 `ConnectionId` 查表。

### 小响应直接写

当连接未关闭、用户态写缓冲为空且没有监听写事件时，`send_in_loop` 可以先尝试直接 `write(fd)`：

1. 一次写完：完成发送；
2. 只写了一部分：剩余数据进入 `writeBuffer_`；
3. 返回 `EAGAIN`：保存剩余数据并关注 `EPOLLOUT`。

高水位只统计用户态 `writeBuffer_` 的数据，已经写入内核发送缓冲区的数据不重复计入。

### 连接生命周期

```text
main loop accept
  → 选择 ioLoop
  → owner loop 创建 TcpConnection
  → 写入本地 ConnectionRecords

连接关闭
  → close callback
  → owner loop 删除 ConnectionRecord
  → 停止 heartbeat

服务器 shutdown
  → 向各 owner loop 投递 force_close
  → activeConnectionCount_ 归零
```

关闭任务中的局部 `TcpConnectionPtr` 在 owner loop 内释放，避免最后一个引用落在主线程，保证 `Channel` 在正确的线程中销毁。

# Result — 结果

- 响应路径不再执行 `ConnectionId` 反向查找；
- 连接记录按 owner loop 分片管理，热路径不需要全局锁；
- `TcpServer` 只负责连接编排、心跳和 shutdown 收口；
- 小响应可以直接写入 fd，减少一次用户态缓冲；
- 当前 benchmark 约从 60w QPS 提升到 80w+ QPS，具体数值仍依赖测试环境。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 方案取舍 | 放弃每次发送都查 `ConnectionId`，回到直接传递连接对象。 |
| 分片连接表 | 按 owner `EventLoop` 分组，局部线程读写。 |
| 发送快路径 | 小响应优先直接 `write(fd)`，失败后进入写缓冲。 |
| 生命周期 | 连接创建、删除和最终析构都回到 owner loop。 |
| shutdown | 主线程投递关闭任务，并等待活跃连接归零。 |

# 面试核心问答总结

## Q1：为什么放弃 ConnectionId 发送接口？

每次发送都需要加锁查表和反向查找，增加热路径开销；双层索引虽然能缓解锁竞争，但会显著增加服务器内部复杂度。

## Q2：如何做到连接表无锁？

按 owner `EventLoop` 分组，连接记录只由所属 loop 线程访问；发送路径也直接回到连接的 owner loop。

## Q3：TcpServer 还保存连接表有什么用？

它不再参与每次发送，而是用于连接生命周期、心跳管理和服务器 shutdown 时统一收口。
