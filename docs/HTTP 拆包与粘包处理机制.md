# HTTP 拆包与粘包处理机制

在 Reactor 事件驱动的非阻塞网络模型中，TCP 字节流是无边界的（Stream-based）。这意味着应用层协议（如 HTTP）必须自行妥善处理以下两种常见网络边界场景：
1. **粘包（Pipelining / Sticky Packets）**：单次 TCP 读取获得了多个完整或部分的 HTTP 请求。
2. **拆包（Fragmentation / Split Packets）**：单个完整的 HTTP 请求被拆分在多次 TCP 读取中到达。

Tudou 在 [HttpServer](file:///home/wxm/Tudou/src/tudou/http/HttpServer.h) 和 [HttpContext](file:///home/wxm/Tudou/src/tudou/http/HttpContext.h) 层面，对这两种场景都实现了完整、高效的流式处理。

---

## 1. 粘包（Packet Sticking）处理方案：单包暂停与消费循环

当对端在同一个 TCP 报文或管道化中接连发送了 `Request_A` 和 `Request_B` 时，网络层读取到的明文数据（`receivedData` 或解密后的 `plaintext`）会包含它们两者的拼接字节。

Tudou 采用 **“单包暂停 + 偏置消费循环”** 来解决此问题：

```text
明文数据流 payload:
┌──────────────────────────────┬──────────────────────────────┐
│  Request_A (40 bytes)        │  Request_B (41 bytes)        │
└──────────────────────────────┴──────────────────────────────┘
 ▲                              ▲
 └── 1. 第一轮 parse 消费 40 字节  └── 2. 重置后第二轮从当前位置起 parse 消费 41 字节
     返回 Complete (暂停)           返回 Complete (再次暂停)
```

### 1.1 协议层：回调中断暂停机制
* **核心代码**：[HttpContext.cpp](file:///home/wxm/Tudou/src/tudou/http/HttpContext.cpp)
* 在 C 语言解析库 `llhttp` 的 `on_message_complete` 静态回调中，当且仅当一个完整的 HTTP 请求行、首部和 Body 解析完毕时，除了标记 `messageComplete_ = true` 外，显式向解析引擎返回 **`HPE_PAUSED`** 状态：
  ```cpp
  int HttpContext::on_message_complete(llhttp_t* parser) {
      ...
      ctx->messageComplete_ = true;
      return HPE_PAUSED; // 强制 llhttp 立即中断，不扫描后面的 Request_B
  }
  ```
* `llhttp_execute` 收到该返回值后立即停止在 `Request_A` 的末尾字符处并返回。

### 1.2 消费度量：查询消耗字节数
* 在 `HttpContext::parse` 中，如果执行结果是 `HPE_PAUSED`，通过 `llhttp_get_error_pos` 计算出已解析的字节大小并记录：
  ```cpp
  const char* errorPos = llhttp_get_error_pos(&parser_);
  consumedBytes_ = static_cast<size_t>(errorPos - data); // 得到 Request_A 真实的 40 字节大小
  ```
* 提供公共 API `get_consumed_bytes()` 供 HttpServer 获取该进度。而在处理下一条请求（调用 `reset()`）时，通过 `llhttp_resume` 将状态机从暂停中恢复。

### 1.3 服务层：平铺指针的 `while` 消化循环
* **核心代码**：[HttpServer.cpp](file:///home/wxm/Tudou/src/tudou/http/HttpServer.cpp)
* `HttpServer::on_message` 获取数据 payload 后，使用一个 `while` 循环：
  ```cpp
  size_t consumed = 0;
  while (consumed < payload.size()) {
      // 1. 切片送入当前解析起点数据
      HttpContext::ParseResult result = state->httpContext.parse(payload.data() + consumed, payload.size() - consumed);
      size_t lastConsumed = state->httpContext.get_consumed_bytes();
      consumed += lastConsumed; // 2. 偏置指针前移

      if (result == HttpContext::ParseResult::Complete) {
          reply_complete_request(conn, *state); // 3. 响应 Request_A 并 reset 解析器
          if (!find_connection_state(conn)) return; // 4. 安全护栏：Connection: close 被断开则直接退出
      }
      ...
  }
  ```
* 由此，`Request_A` 处理并响应后，偏置指针移向 `Request_B`。在下一轮循环中，由于解析器已被重置和恢复，`Request_B` 将被作为一条独立完整的新请求解析并分发。

---

## 2. 拆包（Packet Splitting）处理方案：流式状态机与增量缓冲

当一个请求的 Header 或 Body 跨越了多个 TCP 物理分包时，单次 `conn->receive()` 只能获取到请求的前半部分字节，返回 `NeedMoreData`。

Tudou 采用 **“流式状态机 + 属性增量累加”** 来解决此问题：

```text
第一包: "GET /index.htm" (不完整) ────► 放入 parse ──► 状态机卡在 URL 解析 ──► 返回 NeedMoreData
                                                                         │ (不重置，保留状态)
                                                                         ▼
第二包: "l HTTP/1.1\r\n\r\n"      ────► 放入 parse ──► 状态机无缝拼接完成 ──► 返回 Complete
```

### 2.1 状态机级别：零字节丢弃流式推进
* `llhttp` 状态机在内部维护了当前正处于解析 HTTP 请求行、首部 Key、首部 Value 还是 Body 的状态。
* 在单次 `parse` 扫描到缓冲区末尾但协议未终结时，`llhttp_execute` 返回 `HPE_OK`，`parse` 识别出 `messageComplete_ == false` 并向上返回 `ParseResult::NeedMoreData`。
* 此时，**绝不调用 `HttpContext::reset()`**。

### 2.2 上下文级别：首部与 URL 的增量追加
* 由于 `llhttp` 本身不缓存字符流，`HttpContext` 在回调中对字段进行了增量收集：
  ```cpp
  int HttpContext::on_url(llhttp_t* parser, const char* at, size_t length) {
      ctx->currentUrl_.append(at, length); // 跨分包时，自动把 "l" 拼接到 "/index.htm" 后变成 "/index.html"
      return 0;
  }
  ```
* 所有的首部 Field、Value 以及应用层 Body，均在对应的 `HttpContext` 成员变量中增量追加。
* 下一次 `on_message` 触发时，新的 TCP 报文字节被直接喂给 `state->httpContext.parse(...)`。由于解析状态机和字符缓冲均完整保留，解析器得以完美地无缝续接。

### 2.3 安全传输级（TLS）的非阻塞拆包
* 当启用了 HTTPS 时，拆包可能发生在 TLS 握手记录或 TLS 密文记录层面（例如一个 16KB 的 TLS 密文块分多次到达）。
* `TlsConnection` 采用 Memory BIO 协作：收到的部分密文被写入 `rbio_`。
* `SSL_read` 在解密时，如果发现 `rbio_` 中的密文不足以拼接成一个完整的 TLS 记录（Record），会自动返回 `< 0` 且 `SSL_get_error` 返回 `SSL_ERROR_WANT_READ`。
* `TlsConnection::read_plaintext` 捕获该错误并向 HttpServer 返回 `ReadResult::NeedMoreData`。HttpServer 放弃后续的 HTTP 解析并退出，静待下一个 TCP 可读事件，把新到达的 TLS 密文追加写入 `rbio_`，直至能够解密出明文 HTTP 字节流。

---

## 3. 总结

Tudou 针对非阻塞通信中的流边界问题，构建了高效且安全的防线：
* **粘包防线**：依靠 `on_message_complete` 回调主动返回 `HPE_PAUSED` 实现单请求精准中断，结合外层 `while` 偏置移动循环消化剩余字节。
* **拆包防线**：依靠 `llhttp` 流式状态机与 `HttpContext` 的字符缓冲追加机制，结合 TLS Memory BIO 的 `WANT_READ` 握手机制，实现跨包增量衔接。
