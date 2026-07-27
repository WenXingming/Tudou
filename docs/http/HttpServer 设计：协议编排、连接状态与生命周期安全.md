# HttpServer 设计：协议编排、连接状态与生命周期安全

`HttpServer` 是 HTTP/HTTPS 服务门面：它将 `TcpServer` 的连接、消息、关闭回调编排为“创建连接级协议状态 → 解码请求 → 路由 → 编码响应 → 关闭”的主流程。它不解析 HTTP 字节、不直接调用 OpenSSL，也不管理 fd。

# Situation — 情境

TCP 层只能告诉上层“某条连接建立、收到字节、需要关闭”。HTTP 层还需要为每条连接保存：

- `HttpContext` 的增量解析状态；
- HTTPS 场景下独有的 `TlsConnection` 握手和会话状态。

这些状态不能放进 `TcpConnection`，否则 TCP 层会依赖 HTTP/TLS 协议；也不能放进一个全局临时变量，因为多个 I/O loop 会并行处理不同连接。

# Task — 任务

服务门面需要：

1. 保持 TCP 层与 HTTP/TLS 协议层解耦；
2. 让每条 TCP 连接拥有独立的 `HttpConnection`；
3. 安全管理多 I/O 线程共同访问的连接状态表；
4. 按请求在字节流中的顺序路由和发送响应；
5. 将 HTTP 400、TLS 错误和 `Connection: close` 收敛到明确关闭路径。

# Action — 设计与实现

## `HttpConnection` 是连接级协议边界

```text
TcpConnection：fd、Buffer、Socket I/O、关闭
        │ 一一关联
        ▼
HttpConnection：HttpContext + 可选 TlsConnection
        │
        ▼
HttpServer：路由、响应、连接状态表与回调编排
```

`HttpServer::on_connect()` 在 TCP 连接建立时创建 `HttpConnection`；启用 HTTPS 时，先由 `TlsConfig` 创建独立 `SSL*`，再交给 `TlsConnection`。TLS 会话创建失败则立即关闭 TCP 连接，绝不退化为明文 HTTP。

## 连接状态表：最小 mutex 与脱锁保活

HTTP 连接状态表由多个 I/O loop 的 `on_connect()`、`on_message()`、`on_close()` 访问，因此它不能沿用 TcpServer “每个 EventLoop 独占一份内层 map”的无锁分片策略；这里保留一把只保护 map 的 mutex：

```cpp
std::unordered_map<TcpConnection*, std::shared_ptr<HttpConnection>>
    httpConnections_;
std::mutex contextsMutex_;
```

消息回调查表时，在锁内复制 `shared_ptr`，随后立即释放锁：

```cpp
std::shared_ptr<HttpConnection> httpConnection;
{
    std::lock_guard<std::mutex> lock(contextsMutex_);
    const auto it = httpConnections_.find(conn.get());
    if (it == httpConnections_.end()) {
        return;
    }
    httpConnection = it->second;
}
```

这样锁只保护容器本身；即使随后 `on_close()` 擦除 map 项，当前消息回调仍持有本地 `shared_ptr`，协议状态不会在使用中析构。`HttpConnection` 内部仍只由所属连接的 EventLoop 串行访问，mutex 不用于并发修改解析器或 TLS 会话。

`TcpConnection*` 仅作为 map 身份键，不表达所有权；关闭回调使用同一指针擦除记录。TCP 连接本身由 `TcpServer` 的 owner-loop 连接表持有。

## 消息主流程：读、解码、路由、编码、发送

```text
TcpConnection::receive()
  → 查找并保活 HttpConnection
  → decode_requests()
      ├── TLS 解密 / 握手回包
      └── 提取 0..N 条完整 HttpRequest
  → 按顺序 router_.dispatch()
  → encode_response()
  → TcpConnection::send()
```

核心代码保持按事件发生顺序平铺：

```cpp
const auto result = httpConnection->decode_requests(
    receivedData, requests, outboundCiphertext);
if (!outboundCiphertext.empty()) {
    conn->send(outboundCiphertext);
}

for (const HttpRequest& request : requests) {
    HttpResponse response;
    router_.dispatch(request, response);
    if (send_http_response(conn, *httpConnection, response)) {
        return;
    }
}
```

`outboundCiphertext` 必须先发送，因为它可能是当前 TLS 握手推进产生的必要回包。`requests` 可为空：例如 HTTP 拆包尚未完整，或 HTTPS 握手尚未完成。

## 错误与关闭的收口


| 场景                        | 行为                                                                                             |
| :---------------------------- | :------------------------------------------------------------------------------------------------- |
| `TlsError`                  | 记录错误并`force_close()`。                                                                      |
| `BadRequest`                | 已成功提取的前序 pipeline 请求先完成响应；当前非法请求返回 400，设置`Connection: close` 后关闭。 |
| 响应 TLS 编码失败           | `force_close()`，停止处理后续请求。                                                              |
| 响应包含`Connection: close` | 先发送网络字节，再发起关闭。                                                                     |
| TCP 关闭回调                | 从`httpConnections_` 擦除状态记录。                                                              |

`send_http_response()` 集中“响应编码、发送、关闭语义”。它不会把 `Connection: close` 的协议判断泄漏到 Router 或业务 Handler。

# Result — 结果

- TCP 层保持协议无关，HTTP/TLS 状态集中在 `HttpConnection`；
- 每条连接独立解析、握手，不会互相污染；
- 全局状态表使用最小 mutex，查找后靠 `shared_ptr` 安全脱锁；
- HTTP 与 HTTPS 共用同一条路由和响应主流程；
- pipeline、400、TLS 错误和优雅的 `Connection: close` 都有可追踪的处理路径。

# 测试覆盖


| 测试                                                                              | 验证点                         |
| :---------------------------------------------------------------------------------- | :------------------------------- |
| `HttpServerTest.OnConnectCreatesAndOnCloseRemovesHttpConnection`                  | 建连创建状态、关闭擦除状态。   |
| `HttpServerTest.ProcessPlainHttpRequestDispatchesRegisteredRouteAndSendsResponse` | HTTP 请求完整走通路由与响应。  |
| `HttpServerTest.ProcessPipelinedRequests`                                         | 一批两条请求按顺序路由和响应。 |
| `HttpServerTest.ProcessBadRequestSendsBadRequestAndResetsContext`                 | 非法请求返回 400 并关闭。      |
| `HttpServerTest.TlsReadFailureClosesConnection`                                   | TLS 致命错误后关闭并清理状态。 |

# 核心设计要点提炼


| 设计点   | 说明                                                                    |
| :--------- | :------------------------------------------------------------------------ |
| 分层     | TCP 管字节与 fd；HttpConnection 管单连接协议；HttpServer 管编排与路由。 |
| 连接状态 | 一条`TcpConnection` 对应一份 `HttpConnection`。                         |
| 并发安全 | mutex 只保护全局 map；查表后复制`shared_ptr` 再脱锁。                   |
| 主流程   | 解码请求 → 路由 → 编码响应 → 发送，HTTPS 只是解码/编码内部差异。     |
| 关闭语义 | TLS 错误立即关闭；`Connection: close` 先发送响应再关闭。                |

# 面试核心问答

## Q1：为什么不把 HTTP 状态直接放进 `TcpConnection`？

`TcpConnection` 是通用网络库组件，还要服务 RPC 等其他协议。把 `HttpContext` 或 `TlsConnection` 放进去会让 TCP 层依赖上层协议，破坏分层。`HttpConnection` 是两层之间明确的一对一适配状态。

## Q2：`httpConnections_` 为什么要加 mutex，而 TcpServer 的连接表可以无锁？

TcpServer 的内层 map 按 EventLoop 分片，每份只由所属线程访问；HttpServer 使用一张跨 I/O loop 的全局状态表，建连、收消息、关闭可能并行触及它，所以 mutex 必须存在。锁只保护 map，不保护单连接解析状态。

## Q3：为什么查表后还要复制 `shared_ptr`？

离开锁后另一个回调可能擦除 map 项。复制 `shared_ptr` 让当前 `on_message()` 在处理 TLS、解析和路由期间继续拥有协议状态，避免悬空指针，同时不长时间占用 mutex。

## Q4：HTTP pipeline 出现“前一条合法、后一条非法”时如何处理？

`HttpConnection` 已按顺序输出前面完整的合法请求；HttpServer 先依次响应它们，再对当前非法请求返回 400 并关闭连接，不会丢弃已经解析成功的前序请求。

相关主题：[HttpConnection 设计](<HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换.md>)、[HTTPS 设计](<HTTPS 设计：TlsConfig 与 TlsConnection.md>)、[TcpServer 无锁连接表](<../tcp/TcpServer 设计：按 EventLoop 分片的无 mutex 连接表.md>)。
