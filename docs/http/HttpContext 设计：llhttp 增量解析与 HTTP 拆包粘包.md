# HttpContext 设计：llhttp 增量解析与 HTTP 拆包粘包

`HttpContext` 是单连接的 HTTP/1.x 解析上下文：它持有 `llhttp` 状态机，把无边界的 TCP 明文按请求边界收敛为一个完整的 `HttpRequest`。它不读取 socket、不处理 TLS、不路由，也不发送响应。

# Situation — 情境

TCP 是连续字节流，一次读取不等于一条 HTTP 请求：

- 请求行、Header 或 Body 可能跨多次读取到达；
- 一次读取也可能包含多个完整请求，或“一个完整请求 + 下一个请求的一部分”；
- `llhttp` 对 URL、Header field、Header value 和 Body 的回调同样可能分片。

因此不能把一次 `TcpConnection::receive()` 的结果直接交给 Router；否则会丢失半包状态，或只处理粘在一起的第一个请求。

# Task — 任务

HTTP 解析层需要：

1. 为每条连接保留独立的协议解析状态；
2. 在请求完整时只交付一个稳定的 `HttpRequest`；
3. 准确报告本次输入已消费的字节数，让上层继续处理 pipeline 中的剩余请求；
4. 正确拼接被分片的 URL、Header 和 Body；
5. 在响应、拒绝请求或关闭连接后，不让旧状态泄漏到下一条请求。

# Action — 设计与实现

## 单连接、单线程的解析状态

`HttpServer::ConnectionState` 为每条 `TcpConnection` 持有一个 `HttpContext`。连接的消息回调在其 owner `EventLoop` 中串行执行，因此同一个 `HttpContext` 不在线程间共享，也不需要额外 mutex。

```text
TcpConnection 可读
    → HttpServer::on_message()
    → ConnectionState::httpContext.parse()
    → Router / HttpResponse
```

`HttpContext` 的生命周期与连接级状态一致；它不是无状态工具类，也不应被多个连接复用。

## llhttp 回调桥接

`llhttp` 是 C 库，只接受静态回调。构造时，`HttpContext` 保存回调表并把自身地址写入 `parser_.data`：

```cpp
llhttp_settings_init(&settings_);
settings_.on_url = &HttpContext::on_url;
settings_.on_version_complete = &HttpContext::on_version_complete;
settings_.on_header_field = &HttpContext::on_header_field;
settings_.on_header_value = &HttpContext::on_header_value;
settings_.on_headers_complete = &HttpContext::on_headers_complete;
settings_.on_body = &HttpContext::on_body;
settings_.on_message_complete = &HttpContext::on_message_complete;

llhttp_init(&parser_, HTTP_REQUEST, &settings_);
parser_.data = this;
```

静态回调通过 `parser->data` 恢复当前实例：

```cpp
static HttpContext* get_context(llhttp_t* parser) {
    return static_cast<HttpContext*>(parser->data);
}
```

因此 `HttpContext` 禁止拷贝和移动：对象地址改变会让 `parser_.data` 指向旧地址，后续回调将访问失效对象。

## 按协议顺序构造请求

`HttpContext` 不在消息结束时补写早已收到的字段，而是跟随 `llhttp` 的协议事件逐步构造请求：

```text
on_url
    → 追加 URL 片段，写入 method
on_version_complete
    → 写入 url、path、query、HTTP version
on_header_field / on_header_value
    → 累积 Header 片段
on_headers_complete
    → 提交最后一个 Header
on_body
    → 追加 Body
on_message_complete
    → 暂停解析，交付完整请求
```

`llhttp` 在 `on_version_complete` 前已经完成 HTTP version 的校验和写入，因此此时请求行的 method、request target 与 version 都稳定；`HttpContext` 在这里统一完成请求行字段的写入。

## 拆包：保留解析器与临时字段状态

`parse()` 将本次明文交给同一个 `llhttp` 状态机。输入尚不足以组成完整请求时，`llhttp_execute()` 返回 `HPE_OK`；这表示本次输入都已被接受，但请求仍未结束。

```cpp
if (err == HPE_OK) {
    consumedBytes_ = len;
    return ParseResult::NeedMoreData;
}
```

此时不能调用 `reset()`。`parser_` 已记录协议解析位置，`currentUrl_` 和 Header 缓存也保存了已到达的片段；下一次 `parse()` 会从这里继续。

```text
第 1 次输入：GET /users?na
    currentUrl_ = "/users?na"
    → NeedMoreData

第 2 次输入：me=tom HTTP/1.1\r\nHost: example.com\r\n\r\n
    currentUrl_ = "/users?name=tom"
    → Complete
```

Body 直接以追加方式写入 `HttpRequest`，因此同样支持跨多次回调：

```cpp
int HttpContext::on_body(llhttp_t* parser, const char* at, size_t length) {
    get_context(parser)->request_.append_body(at, length);
    return 0;
}
```

## Header 分片：以“下一个 field 或请求头结束”为提交边界

一个 Header 的 field 和 value 都可能被分片。`HttpContext` 保留三项最小状态：

```text
pendingHeaderField_       尚未提交的 field
pendingHeaderValue_       尚未提交的 value
hasPendingHeaderValue_    当前 field 是否已经收到 value 回调
```

例如 `X-Trace: abc123` 可能产生：

```text
on_header_field("X-Trace")
on_header_value("abc")
on_header_value("123")
on_header_field("Host")
```

不能在每次 `on_header_value()` 后提交，否则第一次回调就会提交 `X-Trace: abc`，清空 field 后无法再正确拼接 `123`。一个 Header 的可靠结束边界只有：收到下一个 field，或请求头结束。

```cpp
int HttpContext::on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    if (ctx->hasPendingHeaderValue_) {
        ctx->commit_pending_header();
    }
    ctx->pendingHeaderField_.append(at, length);
    return 0;
}

void HttpContext::commit_pending_header() {
    if (!hasPendingHeaderValue_) {
        return;
    }

    request_.add_header(pendingHeaderField_, pendingHeaderValue_);
    pendingHeaderField_.clear();
    pendingHeaderValue_.clear();
    hasPendingHeaderValue_ = false;
}
```

只有在已经收到 value 时，新的 `on_header_field()` 才会提交上一项 Header；因此 field 自身被分片时，后续回调只会继续追加。`commit_pending_header()` 仍保留内部条件，作为调用边界的保护。

## 粘包与 HTTP pipeline：暂停、报告消费量、继续解析

`on_message_complete()` 此时不再填写请求字段：请求行、Header 与 Body 已分别在对应的协议回调中构造完毕。它只主动暂停 `llhttp`，把已完成请求的边界交给上层：

```cpp
int HttpContext::on_message_complete(llhttp_t*) {
    return HPE_PAUSED;
}
```

`parse()` 通过 `llhttp_get_error_pos()` 计算暂停点前的消费量，并将暂停结果转换为 `Complete`：

```cpp
if (err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE) {
    const char* errorPos = llhttp_get_error_pos(&parser_);
    consumedBytes_ = errorPos != nullptr
        ? static_cast<size_t>(errorPos - data)
        : len;
    return ParseResult::Complete;
}
```

`HttpServer` 用这个消费量推进偏移，完成一次“解析一个请求 → 路由 → 发送响应 → reset”的循环；若 payload 仍有剩余字节，就继续解析下一条请求：

```cpp
while (consumed < payload.size()) {
    const auto result = state->httpContext.parse(
        payload.data() + consumed, payload.size() - consumed);
    consumed += state->httpContext.get_consumed_bytes();

    if (result == HttpContext::ParseResult::Complete) {
        reply_complete_request(conn, *state);
        continue;
    }
    if (result == HttpContext::ParseResult::NeedMoreData) {
        break;
    }
    // Rejected: 返回 400 并停止本次处理。
}
```

这使一次 TCP 读取中的多个 HTTP 请求按顺序逐个交付，而不会让 Router 看到半条请求。

## reset：一条请求处理后的明确边界

`HttpServer::reply_complete_request()` 发送响应后调用 `HttpContext::reset()`；解析错误时也会先返回 400 再 reset。`reset()` 同时清空 `HttpRequest`、URL/Header 临时状态，并重置/恢复 `llhttp`：

```cpp
void HttpContext::reset() {
    reset_message_state();
    llhttp_reset(&parser_);
    llhttp_resume(&parser_);
    parser_.data = this;
}
```

因此 keep-alive 连接可以复用同一个对象解析下一条请求，同时不会携带上一条请求的 Header、Body 或解析器状态。

# Result — 结果

- 拆包时不丢失 URL、Header、Body 或解析器状态；
- 粘包和 HTTP pipeline 通过 `HPE_PAUSED + consumedBytes_` 按请求边界逐个处理；
- `HttpRequest` 只在 `Complete` 后交给路由层，路由层不需要了解字节流分片；
- Header 分片只保留三项必要状态，没有额外状态机或包装层；
- TLS 只需在 `HttpServer` 中先解密，再将明文交给相同的 `HttpContext`，职责边界清晰。

# 测试覆盖

| 测试 | 验证点 |
| :--- | :--- |
| `HttpContextTest.ParseRequestAcrossMultipleChunks` | Header value 跨输入分片后仍能正确拼接，并能正确解析 Body。 |
| `HttpContextTest.BuildsRequestHeadBeforeBodyCompletes` | 请求头完成而 Body 未到时，method/path/version/Header 已可读取。 |
| `HttpContextTest.ParseSplitRequestLinePreservesTargetAndHttpVersion` | URL、query 与 HTTP version 跨输入分片后仍正确。 |
| `HttpContextTest.ResetDropsPreviousRequestState` | reset 后不会残留上一请求的字段。 |
| `HttpServerTest.ProcessPipelinedRequests` | 一次读取中的两个 HTTP 请求都会被路由和响应。 |

# 面试核心问答

## Q1：为什么不能把一次 read 当成一个 HTTP 请求？

TCP 没有消息边界。一次 read 可能是半个请求、多个请求，或完整请求加下一条请求的一部分；必须由 HTTP 解析器根据协议语法判断边界。

## Q2：拆包时为什么不能 reset `HttpContext`？

`llhttp` 已保存当前解析位置，`HttpContext` 还保存了 URL 和 Header 的片段。reset 会丢掉这些状态，后续字节无法继续构造同一条请求。

## Q3：如何处理一次输入中的多个 HTTP 请求？

第一个请求完成时让 `llhttp` 返回 `HPE_PAUSED`，通过 `llhttp_get_error_pos()` 得到消费量；`HttpServer` 从剩余偏移继续解析。每个请求完成后路由并 reset，再处理下一条。

## Q4：为什么不在每次 Header value 回调后直接提交？

因为 value 也可能分片。提交边界是下一个 Header field 或请求结束；`hasPendingHeaderValue_` 用来区分“field 仍在分片”与“上一组 field/value 已完整，可提交”。

## Q5：为什么 `HttpContext` 禁止拷贝和移动？

`llhttp` 的 `parser_.data` 保存当前对象地址。复制或移动后，该地址可能仍指向旧对象，静态回调恢复上下文时会访问失效内存。

相关主题：[llhttp 选型](<HTTP 解析库选型：为什么选择 llhttp.md>)、[TLS 设计](<HTTPS 安全传输设计：TlsConfig 与 TlsConnection.md>)。
