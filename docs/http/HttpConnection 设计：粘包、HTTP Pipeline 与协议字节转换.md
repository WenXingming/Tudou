# HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换

`HttpConnection` 是 HTTP 层的单连接协议状态。它持有一个 `HttpContext` 和可选的 `TlsConnection`，负责把网络字节转换为按顺序排列的完整 HTTP 请求，并把 HTTP 响应转换回可发送的网络字节；它不拥有 fd、不读取 Socket、不路由请求。

# Situation — 情境

`HttpContext` 能跨多次调用解析一条被拆开的请求，但一次网络读取仍可能包含多条完整 HTTP 请求：

```text
GET /first HTTP/1.1\r\n...\r\n\r\nGET /second HTTP/1.1\r\n...\r\n\r\n
└────────────── 第 1 条 ──────────────┘└────────────── 第 2 条 ──────────────┘
```

这就是应用层常说的粘包，也是 HTTP/1.1 pipeline 的基础。如果只把数据交给 `HttpContext::parse()` 一次，第一条完成后，后面的有效字节无人继续消费。

HTTPS 又增加一层约束：TCP 层收到的是 TLS 密文，必须先解密；握手过程中还可能立刻产生需要反向发送的 TLS 密文，但此时尚未得到任何 HTTP 请求。

# Task — 任务

连接级协议对象需要：

1. 在 HTTP 明文与 HTTPS 密文两种连接上提供同一套请求提取接口；
2. 按字节流顺序提取同一批数据中的所有完整请求；
3. 保留拆包后的 `HttpContext` 状态，等待下一次输入；
4. 将 TLS 握手回包、HTTP 语法错误、TLS 致命错误明确交给 `HttpServer`；
5. 将 `HttpResponse` 序列化并按需加密，而不让 `HttpServer` 了解 OpenSSL 细节。

# Action — 设计与实现

## 一个对象保存一条连接的协议状态

```cpp
class HttpConnection {
private:
    HttpContext httpContext_;
    std::unique_ptr<TlsConnection> tlsConnection_;
};
```

`tlsConnection_ == nullptr` 表示普通 HTTP；存在则表示 HTTPS。`HttpContext` 始终是值成员，因为每条 HTTP 连接都需要它；TLS 会话确实可选，因此使用 `unique_ptr` 表达该差异。

```text
TcpConnection  ── 一一关联 ──>  HttpConnection
                                      ├── HttpContext
                                      └── 可选 TlsConnection
```

## 网络字节先转换为 HTTP 明文

`decode_requests()` 的前半段统一得到 `plaintext`：

```cpp
std::string plaintext;
TlsConnection::ReadResult readResult = TlsConnection::ReadResult::Ready;
if (tlsConnection_) {
    readResult = tlsConnection_->read_plaintext(
        networkData, plaintext, outboundCiphertext);
} else {
    plaintext = networkData;
    outboundCiphertext.clear();
}
```

HTTPS 的 `outboundCiphertext` 是本次读操作触发的握手回包。`HttpServer` 必须先把它交给 `TcpConnection::send()`；即使当前结果是 `Success`，也可能只是“握手尚未完成或明文不足”，并不代表已经得到请求。

## 以消费量循环提取 pipeline 请求

`HttpContext` 每完成一条请求就暂停，并报告本次调用消费量。`HttpConnection` 用该消费量推进偏移：

```cpp
size_t consumed = 0;
while (consumed < plaintext.size()) {
    const auto result = httpContext_.parse(
        plaintext.data() + consumed, plaintext.size() - consumed);
    const size_t lastConsumed = httpContext_.get_consumed_bytes();
    consumed += lastConsumed;

    if (result == HttpContext::ParseResult::Complete) {
        requests.push_back(std::move(httpContext_.get_request()));
        httpContext_.reset();
        continue;
    }
    if (result == HttpContext::ParseResult::Rejected) {
        httpContext_.reset();
        return ProcessResult::BadRequest;
    }
    if (lastConsumed == 0) {
        break;
    }
}
```

关键顺序是“先 move 出完整请求，再 reset 解析器”。`reset()` 会清空当前构建的 `HttpRequest`；完成后继续循环，后续字节才会按线上顺序解析为下一条请求。

`NeedMoreData` 时不 reset。`HttpContext` 保留未完成请求的解析状态，下一次 `decode_requests()` 会从那里继续。

## 用结果类型表达后续动作

| `ProcessResult` | 含义 | `HttpServer` 后续动作 |
| :--- | :--- | :--- |
| `Success` | 本轮处理完成；请求列表可为空或含多条请求 | 发送握手回包，依次路由列表中的请求。 |
| `BadRequest` | HTTP 语法错误 | 已成功提取的前序请求仍先响应；随后返回 400 并关闭。 |
| `TlsError` | TLS 会话不可继续使用 | 关闭 TCP 连接。 |

这比把 TLS、llhttp 和 Socket 错误混在一个布尔返回值中更直接：调用方能据此决定是继续路由、回复 400，还是立即收口连接。

## 响应走反向的单一出口

```cpp
bool HttpConnection::encode_response(
    const HttpResponse& response,
    std::string& networkData) {
    std::string plaintext = response.serialize_to_string();
    if (tlsConnection_) {
        return tlsConnection_->write_plaintext(plaintext, networkData);
    }
    networkData = std::move(plaintext);
    return true;
}
```

HTTP 直接输出序列化结果；HTTPS 交由 TLS 加密。`HttpServer` 只拿到“可发送网络字节”，不用判断连接是否启用了 TLS。

# Result — 结果

- 一次输入中的多条 HTTP 请求会按出现顺序全部提取；
- 一条请求跨多次输入时，解析状态不会被错误重置；
- HTTP 与 HTTPS 共用同一个请求提取和路由流程；
- TLS 握手回包和 HTTP 响应都以网络字节形式回到 `TcpConnection`；
- `HttpServer` 不再直接操作 llhttp 偏移、`reset()` 或 OpenSSL 明文转换细节。

# 测试覆盖

| 测试 | 验证点 |
| :--- | :--- |
| `HttpConnectionTest.PlainConnectionExtractsPipelinedRequestsAndEncodesResponse` | 一批两个请求按顺序提取，HTTP 响应直接编码。 |
| `HttpConnectionTest.PlainConnectionCompletesRequestAcrossNetworkChunks` | 拆包后下一次输入可完成原请求。 |
| `HttpConnectionTest.PlainConnectionReportsBadRequest` | 非法 HTTP 返回 `BadRequest`。 |
| `HttpServerTest.ProcessPipelinedRequests` | 两条粘连请求均被路由并产生响应。 |

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 连接级状态 | 每条连接独占 `HttpContext` 与可选 TLS 会话。 |
| 粘包处理 | 依据 `consumedBytes_` 循环，而不是假定一次读取只有一条请求。 |
| pipeline 顺序 | 每条完成请求先 move、再 reset、再处理剩余字节。 |
| HTTP/HTTPS 统一 | 输入先得到明文，输出统一得到网络字节。 |
| 错误分流 | HTTP 语法错误与 TLS 致命错误由不同结果明确区分。 |

# 面试核心问答

## Q1：`HttpContext` 和 `HttpConnection` 分别解决什么问题？

`HttpContext` 解决一条请求的增量解析与拆包；`HttpConnection` 解决一批数据中多条请求的粘包/pipeline 提取，并统一 HTTP 与 HTTPS 的字节转换。

## Q2：为什么第一个请求完成后不能直接丢弃本次剩余数据？

剩余数据可能已经是下一条合法 HTTP 请求。通过 `consumedBytes_` 推进偏移，才能避免漏处理 pipeline 请求。

## Q3：TLS 握手尚未完成时为什么 `Success` 但没有请求？

这表示当前网络密文已被 TLS 会话正常接受，但尚不足以完成握手或解出 HTTP 明文；连接必须保留状态并等待下一次输入，而不是把它当作 HTTP 错误。

## Q4：为什么 `HttpServer` 不直接调用 `HttpContext::parse()`？

那会让服务门面同时了解 TLS、llhttp 消费偏移、请求 move/reset 和 pipeline 循环。`HttpConnection` 将这些单连接协议细节收口后，`HttpServer` 只编排“解出请求 → 路由 → 发送响应”。

相关主题：[HttpContext 设计](<HttpContext 设计：llhttp 增量解析与 HTTP 拆包.md>)、[HTTPS 设计](<HTTPS 设计：TlsConfig 与 TlsConnection.md>)、[HttpServer 设计](<HttpServer 设计：协议编排、连接状态与生命周期安全.md>)。
