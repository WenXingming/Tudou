# TODO：HttpServer 连接泄露与优雅半关闭局限性分析

本文记录两个相关问题：HTTP `Connection: close` 是否真正关闭连接，以及当前 Tudou 缺少优雅半关闭状态机带来的大响应截断风险。

# Situation — 情境

`HttpResponse` 可以设置 `closeConnection_`，序列化后会输出 `Connection: close`。如果 `HttpServer` 发送响应后不读取这个标记，连接仍可能保持 Keep-Alive，最终积累为连接和 fd 泄露。

即使补上关闭动作，当前 `force_close()` 也是立即关闭路径。当响应还有数据停留在应用层写缓冲区时，直接关闭会丢失这些数据。

# Task — 任务

需要区分两个层次的问题：

1. 发送 `Connection: close` 响应后，确实关闭对应连接；
2. 关闭前先刷完写缓冲，避免大响应被截断。

# Action — 当前方案与局限

### 修复 Connection: close 的连接泄露

发送响应后检查关闭标记：

```cpp
const auto connectionHeader = resp.get_headers().find("Connection");
if (connectionHeader != resp.get_headers().end() && connectionHeader->second == "close") {
    conn->force_close();
}
```

这可以修复“响应要求关闭，但服务器继续保持连接”的问题。

### 当前 force_close 的行为

当前 `force_close()` 会立即执行关闭流程：

1. 调用 `shutdown_write()`；
2. `disable_all()`，停止继续监听读写事件；
3. 触发关闭回调，移除连接。

小响应通常已经完整写入内核发送缓冲区，因此能够正常到达客户端。但大响应可能还有数据停留在 `writeBuffer_`，`disable_all()` 后没有机会继续监听可写事件，这部分数据会被丢弃。

### 理想的优雅半关闭

完整的半关闭状态机通常是：

```text
请求要求关闭
  → 标记 Disconnecting
  → 继续监听写事件并刷空 writeBuffer_
  → shutdown(fd, SHUT_WR)
  → 等待读端 EOF
  → 最终 close(fd) 并回收对象
```

# Result — 当前结果

- `Connection: close` 的连接泄露可以通过发送后调用 `force_close()` 修复；
- 当前关闭路径适合小响应和立即终止场景；
- 大响应在写缓冲未清空时存在截断风险；
- 真正解决该风险需要增加 `kConnected`、`kDisconnecting`、`kDisconnected` 等状态和异步 `shutdown()`。

# 未来优化路线

1. 为 `TcpConnection` 增加关闭状态机；
2. 实现非阻塞 `shutdown()`，写缓冲未清空时只改变状态；
3. 在 `on_write()` 刷空缓冲后执行 `shutdown_write()`；
4. 将 HTTP 的 `force_close()` 场景迁移到优雅关闭接口；
5. 为大响应、TLS 响应和连接关闭并发增加测试。

# 面试核心问答总结

## Q1：为什么输出 Connection: close 后还要显式关闭连接？

响应头只是协议层意图，不会自动关闭服务器的 socket。服务器必须在发送完成后执行对应关闭逻辑。

## Q2：force_close 为什么可能截断大响应？

如果数据还在应用层 `writeBuffer_`，立即 `disable_all()` 会停止后续可写事件，剩余数据没有机会发送。

## Q3：优雅半关闭解决什么问题？

它允许连接先刷完待发送数据，再发送 FIN，避免关闭动作丢失大响应。
