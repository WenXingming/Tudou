# HTTP 拆包与粘包处理机制

TCP 只提供无边界字节流，HTTP 请求的边界必须由应用层根据协议解析结果自行确定。Tudou 使用 `llhttp` 和 `HttpContext` 处理不完整请求，也处理一次读取中包含多个请求的情况。

# Situation — 情境

一次 TCP 读取可能得到：

- 一个请求的一部分（拆包）；
- 多个完整请求；
- 一个完整请求加上下一个请求的一部分（粘包与拆包同时发生）。

因此，网络层不能把一次 `read` 直接当成一个 HTTP 请求。

# Task — 任务

HTTP 层需要：

1. 保存跨多次读取的解析状态；
2. 在请求完整时准确计算已经消费的字节数；
3. 一次读取包含多个请求时逐个处理；
4. TLS 密文不完整时保留状态，等待后续数据；
5. 避免已关闭连接在处理批量请求时继续访问。

# Action — 处理方案

### 粘包：暂停解析并按消费量循环

当一段明文中包含 Request A 和 Request B 时，`HttpContext` 在第一个请求完成的回调中暂停 `llhttp`：

```cpp
int HttpContext::on_message_complete(llhttp_t* parser) {
    ctx->messageComplete_ = true;
    return HPE_PAUSED;
}
```

解析器暂停在 Request A 的末尾，通过错误位置计算本次消费的字节数：

```cpp
const char* errorPos = llhttp_get_error_pos(&parser_);
consumedBytes_ = static_cast<size_t>(errorPos - data);
```

`HttpServer` 根据消费量移动偏移量，处理剩余字节：

```cpp
size_t consumed = 0;
while (consumed < payload.size()) {
    auto result = context.parse(
        payload.data() + consumed,
        payload.size() - consumed);

    consumed += context.get_consumed_bytes();

    if (result == HttpContext::ParseResult::Complete) {
        reply_complete_request(conn, state);
        if (!find_connection_state(conn)) {
            return;
        }
    } else {
        break;
    }
}
```

这样 Request A 完成后，下一轮从正确偏移处继续解析 Request B。

### 拆包：保留状态并增量追加

如果请求只到达了一部分，`llhttp` 返回“还需要更多数据”。此时不能调用 `reset()`，而应保留解析状态和已收集的字段：

```text
第一包: "GET /index.htm"
  → 状态停在 URL 中间
  → 返回 NeedMoreData
  → 保留解析器状态和已收集字段

第二包: "l HTTP/1.1\r\n\r\n"
  → 继续执行
  → 返回 Complete
```

HTTP 首部、URL 和 Body 在回调中增量追加：

```cpp
int HttpContext::on_url(
    llhttp_t* parser, const char* at, size_t length) {
    ctx->currentUrl_.append(at, length);
    return 0;
}
```

### TLS 下的拆包

HTTPS 需要先处理 TLS 记录，再把解密后的明文交给 HTTP 解析器：

```text
TCP 密文
  → 写入 TlsConnection::rbio_
  → SSL_read / SSL_do_handshake
  → WANT_READ：继续等待密文
  → 得到明文后交给 HttpContext
```

TLS 密文不完整时返回 `NeedMoreData`，不重置 `rbio_` 或 HTTP 上下文，下一次可读事件继续处理。

# Result — 结果

- 多个 HTTP 请求可以在一次读取中按边界逐个处理；
- 不完整的请求会保留状态，等待后续字节；
- HTTP 和 TLS 各自处理自己的流边界；
- 连接关闭时会检查连接状态，避免批量解析期间继续访问已销毁对象。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| TCP 无边界 | 不能把一次 `read` 当成一个 HTTP 请求。 |
| 粘包处理 | `HPE_PAUSED` 暂停当前请求，按消费量继续处理剩余字节。 |
| 拆包处理 | 保留解析器状态和字段缓存，等待下一次数据。 |
| TLS 协作 | Memory BIO 返回 `WANT_READ` 时保留密文状态。 |

# 面试核心问答总结

## Q1：为什么一次 read 不能对应一个 HTTP 请求？

TCP 是字节流协议，没有消息边界。一次 read 可能得到半个请求，也可能得到多个请求。

## Q2：Tudou 如何处理一次读取中的多个 HTTP 请求？

请求完成时让 `llhttp` 暂停，计算已经消费的字节数，再由外层循环从剩余偏移处继续解析。

## Q3：拆包时为什么不能 reset 解析器？

因为解析器已经保存了当前状态，首部和 URL 也可能只收到一部分。reset 会丢失这些信息，后续数据无法接着解析。
