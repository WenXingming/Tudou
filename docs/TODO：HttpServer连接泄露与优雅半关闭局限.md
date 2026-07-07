# TODO：HttpServer 连接泄露与优雅半关闭局限性分析

## 1. 核心问题：Connection: close 响应后的连接泄露

### 1.1 问题现状
在 [HttpResponse](file:///home/wxm/Tudou/src/tudou/http/HttpResponse.h) 中设计了 `closeConnection_` 属性。当返回 400 Bad Request、404 Not Found、405 Method Not Allowed 或业务层设置了该标记时，序列化出的响应头会包含 `Connection: close`。
然而，在 [HttpServer.cpp](file:///home/wxm/Tudou/src/tudou/http/HttpServer.cpp) 原有实现中，服务器发送完响应后，完全没有读取或判断该标记，导致连接始终保持在物理开启状态。如果客户端不主动断开（或处于 Keep-Alive 状态），会造成服务端连接泄露和文件描述符耗尽。

### 1.2 解决方案
在 `HttpServer::send_http_response` 中发送报文完毕后，显式检测该标记并调用断开逻辑：
```cpp
if (resp.get_close_connection()) {
    conn->force_close();
}
```

---

## 2. 架构局限性：缺乏优雅半关闭（Graceful Shutdown）状态机

由于 Tudou 网络库本身采用了极简设计，其 `TcpConnection` 和 `TlsConnection` 并没有完整实现类似于 Muduo 网络库的“两阶段优雅关闭（Half-Close）”状态机：

### 2.1 优雅关闭的理想设计（如 Muduo）
1. **第一阶段：应用层半关闭**  
   当发送完响应且需要关闭连接时，调用 `conn->shutdown()`。
   * 若当前发送缓冲区 `writeBuffer_` 仍有残留数据积压，则只将连接状态标记为 `kDisconnecting`，继续注册并监听写事件，让 EventLoop 异步刷干缓冲区。
   * 当缓冲区完全清空（`writeBuffer_->readable_bytes() == 0`）后，才真正调用 `::shutdown(fd, SHUT_WR)` 向对端发送 FIN 报文。
2. **第二阶段：对端确认与物理回收**  
   对端收到 FIN 报文后，读取到 EOF (返回 `0` 字节)，随后对端也关闭其写端并发送 FIN。
   本端接收到对端的 FIN 报文后，触发物理上的 `close(fd)` 并回收 C++ 连接对象。

### 2.2 Tudou 目前的局限性与风险
Tudou 目前仅对外暴露了 `force_close()` 接口。该接口会立刻执行 `close_connection`，调用 `connSocket_.shutdown_write()` 并**立刻反注册所有 Poller 事件（`disable_all()`）**。
这带来了一个潜在的截断风险：
* **小报文场景（常见）**：如果 HTTP 响应很小（如默认的 400/404 响应），数据在 `send()` 时已经一次性完整写入了操作系统的内核发送缓冲区，此时立刻调用 `force_close()` 会安全发送 FIN，客户端能接收到完整报文。
* **大报文场景（局限）**：如果响应非常大（如大文件或大 JSON），导致 `send()` 时有一部分数据被迫滞留在应用层 `writeBuffer_` 中，而我们立刻调用了 `force_close()`，由于 `disable_all()` 掐断了后续的写事件触发，**这部分滞留在应用层写缓冲里的数据将被直接丢弃**，导致客户端收到截断的数据报错。

---

## 3. 未来优化路线

要彻底解决上述局限性，需要在传输层重构支持优雅半关闭：
1. **状态机拓展**：为 `TcpConnection` 引入 `StateE` 状态（`kConnected`、`kDisconnecting`、`kDisconnected`）。
2. **非阻塞优雅 Shutdown**：在 `TcpConnection` 中实现 `shutdown()` 接口。在发送缓冲未清空前只标记状态，在写回调（`on_write`）刷空缓冲区后再执行 `shutdown_write`。
3. **HTTP 联动**：将 `HttpServer` 中的 `conn->force_close()` 升级替换为上述优雅 `conn->shutdown()`。
