# HTTP 解析库选型：为什么选择 llhttp

本文说明 Tudou 为什么选择 `llhttp` 作为 HTTP/1.x 解析器，以及它如何与现有的 Reactor 网络层配合。

# Situation — 情境

Tudou 已经有基于 Reactor 的非阻塞 TCP 网络层，但网络层收到的仍然只是字节流。HTTP 服务还需要一个解析器，把解密后的 HTTP 明文转换为请求方法、路径、首部和消息体等结构化数据。

如果自己实现完整的 HTTP 状态机，需要处理持久连接、分块传输、非法报文和大量边界条件，维护成本较高。

# Task — 任务

解析库需要满足：

1. 只负责 HTTP 解析，不绑定具体网络 I/O 或线程模型；
2. 支持流式、增量解析，适配非阻塞 socket；
3. 覆盖 HTTP/1.x 的常用语义，包括 Keep-Alive 和 Chunked；
4. 性能足够高，不能成为请求处理的主要瓶颈；
5. 便于通过回调接入 Tudou 的 `HttpContext`。

# Action — 选型与实现

### 候选方案比较

- `cpp-httplib`：接口简单，但同时包含网络和 HTTP 能力，与 Tudou 已有的网络层重复；
- `picohttpparser`：体积小、速度快，但功能更精简，需要自行补充更多协议处理；
- `http-parser`：成熟稳定，但项目已经归档，后续维护有限；
- `llhttp`：延续 `http-parser` 的状态机思路，生成 C 代码，接口保持纯解析器形态，适合嵌入现有网络层。

因此选择 `llhttp`，核心原因不是“功能最多”，而是它在职责边界、性能和协议覆盖之间更符合 Tudou 的架构。

### 与 Reactor 网络层的协作

TcpConnection 负责读取字节，HTTP 上下文保存解析状态，`llhttp_execute()` 每次处理当前缓冲区中的数据：

```text
socket 可读
  → TcpConnection 读取字节到 Buffer
  → HttpConnection 完成 HTTP 明文透传或 TLS 解密
  → HttpContext 调用 llhttp 增量解析
  → llhttp 回调填充 HttpRequest
  → HttpConnection 提取完整请求后交给 Router
```

解析器不关心数据来自 socket、TLS 还是测试字符串，只处理传入的字节和回调。

### 选型取舍

`llhttp` 解决的是 HTTP 语法解析，不负责路由、连接管理或业务处理。这样可以保持模块边界清晰：

- `TcpConnection`：处理字节流和读写事件；
- `HttpConnection`：保存单连接协议状态，负责 HTTP/HTTPS 字节转换与 pipeline 请求提取；
- `HttpContext`：保存一条请求的增量解析状态；
- `llhttp`：识别 HTTP 报文并触发回调；
- `Router`：分发完成的 `HttpRequest`。

# Result — 结果

- HTTP 解析从网络层独立出来；
- 增量解析适配非阻塞读和 TCP 拆包；
- 常用 HTTP/1.x 语义由成熟状态机处理；
- 后续路由和业务逻辑不需要了解解析器内部细节。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 选型原则 | 优先选择与现有网络层职责互补的纯解析器。 |
| 增量解析 | 每次处理当前可用字节，适配 TCP 拆包和非阻塞 I/O。 |
| 模块边界 | `llhttp` 只解析 HTTP，不负责网络、路由和业务。 |
| 最终选择 | 在功能、性能和维护状态之间选择 `llhttp`。 |

# 面试核心问答总结

## Q1：为什么不自己实现 HTTP 解析器？

HTTP 状态机需要处理大量边界条件。使用成熟解析器可以减少协议实现风险，自己只需要负责缓冲区和业务回调的连接。

## Q2：为什么不用 cpp-httplib？

它同时包含网络和 HTTP 能力，而 Tudou 已经有自己的 Reactor 网络层。引入后会产生功能重复和架构耦合。

## Q3：为什么 llhttp 适合非阻塞网络？

它是流式状态机，可以多次输入不完整数据，等后续字节到达后继续解析，适合 TCP 拆包和非阻塞读取。

相关主题：[HttpContext 设计](<HttpContext 设计：llhttp 增量解析与 HTTP 拆包.md>)、[HttpConnection 设计](<HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换.md>)。
