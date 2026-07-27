# HTTP/TLS 模块

## 模块职责

HTTP 层在 TCP 字节流之上完成 HTTP/1.x 解析、请求上下文、路由、响应和 TLS 传输。TLS 类位于 `src/tudou/http`，因为它服务于 HTTP 服务生命周期。

## 核心对象

| 类 | 职责 |
| --- | --- |
| `HttpServer` | 绑定 TcpServer 回调，驱动请求解析和响应发送 |
| `HttpContext` | 按连接维护 llhttp 解析状态和请求边界 |
| `HttpRequest` / `HttpResponse` | 表达协议数据，不管理 socket |
| `HttpRouter` | 按 method/path 分发业务处理器 |
| `TlsConfig` / `TlsConnection` | 管理 OpenSSL 配置和单连接 TLS 状态 |

## 核心流程

```text
TcpConnection::on_read
  → HttpServer::on_message
  → HttpContext::parse
  → HttpServer::on_http_request
  → HttpRouter::dispatch
  → HttpResponse::package_to_string
  → TcpConnection::send
```

HTTPS 连接在 TCP 回调和 HTTP 解析之间增加 TLS 明文/密文转换，但仍遵守同一个 owner EventLoop。

## 当前取舍

- 使用 llhttp 处理 HTTP 流式解析，应用层不自行处理粘包和拆包。
- 路由顺序为精确匹配、405 判定、前缀路由、404 回退。
- 响应统一使用内存 body；当前不包含 `sendfile`、kTLS 零拷贝和文件 body 特殊路径。

## 深入文档

- [HTTP 拆包与粘包](<HTTP 拆包与粘包处理机制.md>)
- [llhttp 选型](<HTTP 解析库选型：为什么选择 llhttp.md>)
- [Router 设计](<路由模块设计：高效的请求分发.md>)
- [TLS 配置与连接职责](<HTTPS 安全传输设计：TlsConfig 与 TlsConnection.md>)
