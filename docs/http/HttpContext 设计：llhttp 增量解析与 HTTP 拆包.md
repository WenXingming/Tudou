# HttpContext 设计：llhttp 增量解析与 HTTP 拆包

`HttpContext` 是单条 HTTP 请求的增量解析上下文。它持有 `llhttp` 状态机和正在构建的 `HttpRequest`，把跨多次到达的 HTTP 明文收敛为一条完整请求；它不读取 Socket、不处理 TLS，也不负责同一批数据中多条请求的提取。

# Situation — 情境

TCP 是连续字节流。一次读取可能只到达请求行、Header 或 Body 的一部分；甚至 `llhttp` 对 URL、Header field、Header value 的回调也可能再次分片。

```text
第 1 次明文：POST /orders HTTP/1.1\r\nHost: exa
第 2 次明文：mple.com\r\nContent-Length: 5\r\n\r\nhello
```

如果每次读取都重新创建解析器，第一段已识别出的 method、URL、Header 片段会丢失；如果在 Header value 首次回调时立即提交，又会把被拆开的 value 错误地当成完整字段。

# Task — 任务

解析上下文需要：

1. 为当前请求保留 `llhttp` 和临时字段状态；
2. 接受任意边界的明文片段，并在数据不足时继续等待；
3. 只在一条请求语义完整后交付 `HttpRequest`；
4. 告知上层本次输入消费了多少字节；
5. 请求完成或被拒绝后可明确复位，避免状态泄漏到下一条请求。

# Action — 设计与实现

## 单请求、单连接的状态

`HttpConnection` 为每条 TCP 连接持有一个 `HttpContext`。同一连接的消息回调始终由 owner `EventLoop` 串行执行，因此 `HttpContext` 不在线程间共享，也不需要锁。

```text
TcpConnection 收到网络字节
  → HttpConnection 解密（HTTPS）或透传（HTTP）
  → HttpContext::parse()
  → 一条完整 HttpRequest
```

`llhttp_t::data` 保存当前 `HttpContext` 地址，静态回调通过它恢复 C++ 对象：

```cpp
llhttp_init(&parser_, HTTP_REQUEST, &settings_);
parser_.data = this;

static HttpContext* get_context(llhttp_t* parser) {
    return static_cast<HttpContext*>(parser->data);
}
```

因此类禁止拷贝和移动。对象地址改变后，`parser_.data` 仍可能指向旧对象，后续回调会访问失效内存。

## 按协议完成点写入请求行

`on_url()` 只累积 request target 片段；method、URL、path、query、version 都在 `on_version_complete()` 一次写入：

```cpp
int HttpContext::on_url(llhttp_t* parser, const char* at, size_t length) {
    get_context(parser)->currentUrl_.append(at, length);
    return 0;
}

int HttpContext::on_version_complete(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    ctx->request_.set_method(llhttp_method_name(
        static_cast<llhttp_method>(parser->method)));
    ctx->request_.set_url(ctx->currentUrl_);
    // 由 currentUrl_ 写入 path/query，由 parser 写入 HTTP version。
    return 0;
}
```

这样每个回调只处理已完整的协议语义：URL 分片不会重复写入 request，version 尚未确定时也不会提前提交请求行。

## Header 分片的最小临时状态

一个 Header 的 field/value 都可能被拆开。当前实现只保留：

```text
pendingHeaderField_       尚未提交的字段名片段
pendingHeaderValue_       尚未提交的字段值片段
hasPendingHeaderValue_    是否已经开始接收当前字段的 value
```

不能在每次 `on_header_value()` 后提交，因为 value 可能继续到达。可靠提交边界是“收到下一项 field”或“Header 区结束”：

```cpp
int HttpContext::on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    if (ctx->hasPendingHeaderValue_) {
        ctx->commit_pending_header();
    }
    ctx->pendingHeaderField_.append(at, length);
    return 0;
}

int HttpContext::on_headers_complete(llhttp_t* parser) {
    get_context(parser)->commit_pending_header();
    return 0;
}
```

Body 直接追加到 `request_`，同样天然支持跨多次输入。

## 用暂停点交付一条完整请求

请求完成时，`on_message_complete()` 返回 `HPE_PAUSED`。`parse()` 将其转换成 `Complete`，并通过 `llhttp_get_error_pos()` 记录该请求在本次输入中消费的字节数：

```cpp
if (err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE) {
    const char* errorPos = llhttp_get_error_pos(&parser_);
    consumedBytes_ = errorPos != nullptr
        ? static_cast<size_t>(errorPos - data)
        : len;
    return ParseResult::Complete;
}
```

`HttpContext` 到此只交付一条请求。剩余字节属于 `HttpConnection` 的职责：它根据 `consumedBytes_` 移动偏移、取走请求、`reset()` 后再解析下一条，详见 [HttpConnection 设计](<HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换.md>)。

## 拆包与 reset 的边界

输入不足时，`llhttp_execute()` 返回 `HPE_OK`：本次字节已被解析器接受，但请求还未结束。

```cpp
if (err == HPE_OK) {
    consumedBytes_ = len;
    return ParseResult::NeedMoreData;
}
```

此时不能 `reset()`；`parser_`、`currentUrl_` 和 Header 缓存正是等待下一段数据所需的状态。只有上层已经移走完整请求，或当前请求被拒绝时，才调用：

```cpp
void HttpContext::reset() {
    reset_message_state();
    llhttp_reset(&parser_);
    llhttp_resume(&parser_);
    parser_.data = this;
}
```

# Result — 结果

- 请求行、Header、Body 可跨任意次数输入到达；
- Router 只会收到已经完整的 `HttpRequest`；
- Header 分片只引入三项必要临时状态，没有额外状态机；
- `consumedBytes_` 把“单请求解析”与“多请求提取”分成明确边界；
- keep-alive 连接可以复用同一个解析器，而不会携带上一请求状态。

# 测试覆盖

| 测试 | 验证点 |
| :--- | :--- |
| `HttpContextTest.ParseRequestAcrossMultipleChunks` | Header value 与 Body 跨输入分片后仍正确。 |
| `HttpContextTest.BuildsRequestHeadBeforeBodyCompletes` | 请求头完成、Body 未到时，已完成字段可读。 |
| `HttpContextTest.ParseSplitRequestLinePreservesTargetAndHttpVersion` | URL 与 HTTP version 分片后仍正确。 |
| `HttpContextTest.ResetDropsPreviousRequestState` | reset 后不残留上一请求字段。 |

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 增量状态 | `llhttp` 与临时字段状态留在同一 `HttpContext`，等待后续片段。 |
| 请求行提交点 | URL 只累积；method/url/path/query/version 在 version 完成时一起写入。 |
| Header 提交点 | 下一个 field 或 Header 结束，不能假设 value 回调一次完整。 |
| 完成边界 | `HPE_PAUSED` 表示一条请求完成，`consumedBytes_` 交给上层。 |
| 生命周期 | `parser_.data` 指向对象自身，因此禁止拷贝与移动。 |

# 面试核心问答

## Q1：为什么不能把一次 read 当成一条 HTTP 请求？

TCP 没有消息边界。一次 read 可能是半条请求，也可能只包含某个 Header value 的一部分；必须保留解析状态，等待协议语法确认请求结束。

## Q2：为什么 Header value 不能一收到就写入 `HttpRequest`？

因为同一个 value 可以被 `llhttp` 分成多次回调。当前实现以“下一个 field 或 Header 结束”作为提交边界，保证 field/value 都已完整。

## Q3：`HttpContext` 如何知道一条请求结束？

`on_message_complete()` 主动让 llhttp 暂停，`parse()` 把 `HPE_PAUSED` 转换为 `Complete`，并记录暂停点前的消费字节数。

## Q4：为什么禁止移动 `HttpContext`？

llhttp 的 C 回调通过 `parser_.data` 保存对象地址。移动会让解析器仍指向旧地址，破坏回调安全性。

相关主题：[HTTP 解析库选型](<HTTP 解析库选型：为什么选择 llhttp.md>)、[HttpConnection 设计](<HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换.md>)。
