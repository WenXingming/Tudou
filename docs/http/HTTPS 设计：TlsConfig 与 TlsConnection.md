# HTTPS 设计：TlsConfig 与 TlsConnection

Tudou 将 OpenSSL 分为服务器级 `TlsConfig` 与连接级 `TlsConnection`：前者持有共享 `SSL_CTX`，后者持有每条连接独有的 `SSL` 及其 Memory BIO。Socket I/O 始终留在 Reactor/TCP 层，TLS 只负责密文与明文转换。

# Situation — 情境

HTTPS 同时包含两类完全不同的状态：

- 证书、私钥、最低协议版本等服务器级配置；
- TLS 握手进度、会话密钥、已解密字节等连接级状态。

如果每条连接都重复加载证书，会浪费资源且混淆生命周期；如果用 `SSL_set_fd()` 让 OpenSSL 直接读写 Socket，则会绕过 `EventLoop` 对非阻塞 I/O、写缓冲和关闭路径的统一管理。

# Task — 任务

HTTPS 接入需要：

1. 服务器级配置只初始化一次，并能为新连接创建独立会话；
2. 每条连接独立推进握手和加解密，不能跨连接共享 `SSL`；
3. OpenSSL 不直接操作 Socket，不引入阻塞读写；
4. 握手产生的回包与 HTTP 响应都回到 `TcpConnection::send()`；
5. TLS 致命错误能够明确收口 TCP 连接。

# Action — 设计与实现

## 配置与会话分离

| 组件 | 持有资源 | 职责 | 生命周期 |
| :--- | :--- | :--- | :--- |
| `TlsConfig` | `SSL_CTX*` | 加载 PEM 证书/私钥，设置 TLS 1.2 最低版本，创建 `SSL*` | `HttpServer` 级别 |
| `TlsConnection` | `SSL*` 与其读写 Memory BIO | 服务端握手、解密输入、加密输出 | 单条 `HttpConnection` 级别 |

初始化失败时 `TlsConfig::init()` 会释放刚创建的上下文，使对象回到未初始化状态：

```cpp
ctx_ = SSL_CTX_new(TLS_server_method());
SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
SSL_CTX_use_certificate_file(ctx_, certFile.c_str(), SSL_FILETYPE_PEM);
SSL_CTX_use_PrivateKey_file(ctx_, keyFile.c_str(), SSL_FILETYPE_PEM);
SSL_CTX_check_private_key(ctx_);
```

每次新连接由 `create_ssl_session()` 创建独立 `SSL*`，随后转交给 `TlsConnection`。`SSL` 的最终释放权属于 `TlsConnection`。

## Memory BIO：让 TLS 成为内存过滤器

`TlsConnection` 创建读、写 Memory BIO 后，通过 `SSL_set_bio()` 交给 OpenSSL；BIO 的所有权随 `SSL_free()` 一起释放。类不保存独立 `rbio_` / `wbio_` 成员，需要时经 `SSL_get_rbio()` / `SSL_get_wbio()` 访问。

```text
网络密文
  → TcpConnection::receive()
  → BIO_write(SSL_get_rbio(...))
  → SSL_do_handshake / SSL_read
  → HTTP 明文

HTTP 明文
  → SSL_write
  → BIO_read(SSL_get_wbio(...))
  → 网络密文
  → TcpConnection::send()
```

这样 OpenSSL 从未直接读写 fd；EventLoop 仍唯一负责 socket 事件、非阻塞短写与关闭。

## 输入路径：握手、解密与握手回包

`read_plaintext()` 的顺序固定：

```text
网络密文写入 rbio
  → 未握手完成则 SSL_do_handshake
  → 先取出本轮 wbio 中的待发密文
  → 握手完成后循环 SSL_read 取尽当前 HTTP 明文
```

```cpp
if (!is_established() && !advance_handshake()) {
    return ReadResult::Error;
}

outboundCiphertext = drain_ciphertext();
if (!is_established()) {
    return ReadResult::NeedMoreData;
}
```

`WANT_READ` / `WANT_WRITE` 不是失败：表示握手或解密尚未完成。当前实现保留 `SSL` 状态、取出本轮已产生的输出并返回 `NeedMoreData`；`HttpConnection` 不会把握手密文误交给 HTTP 解析器。

## 输出路径：响应明文到网络密文

`HttpConnection::encode_response()` 先序列化 `HttpResponse`。HTTPS 连接调用 `TlsConnection::write_plaintext()`，后者执行 `SSL_write()` 并 drain 写 BIO；普通 HTTP 直接返回序列化结果。

```text
HttpResponse
  → serialize_to_string()
  → SSL_write()
  → drain_ciphertext()
  → TcpConnection::send()
```

TLS 会话已失效、尚未完成握手却要求加密、或 `SSL_write()` 失败时，编码返回 `false`；`HttpServer` 随即 `force_close()`，不会保留一条之后只能持续报错的连接。

## 单一路径与边界

当前只保留 Memory BIO 路径：没有 kTLS、没有 sendfile 特例、没有握手后切换传输模式。这样 `TcpConnection` 始终只处理字节，HTTP 始终只处理明文协议，TLS 的职责只有“密文 ↔ 明文”。

未来若有明确的 HTTPS 大文件吞吐需求，可将 kTLS 作为独立优化重新评估；它不属于当前实现能力。

# Result — 结果

- `SSL_CTX` 与 `SSL` 的生命周期按服务器/连接层级清晰分离；
- TLS 不绕过 Reactor，Socket 读写仍集中于 TCP 层；
- 握手回包与业务响应走同一条 `TcpConnection::send()` 路径；
- HTTP 层只接触解密后的明文，不需要了解 OpenSSL BIO；
- TLS 致命错误会释放会话并关闭连接。

# 测试覆盖

| 测试 | 验证点 |
| :--- | :--- |
| `TlsConfigTest.InitWithValidCertificateCreatesServerSsl` | 证书/私钥初始化后可创建服务端 `SSL`。 |
| `TlsConfigTest.FailedReinitializationClearsPreviousContext` | 重初始化失败后不保留旧 `SSL_CTX`。 |
| `TlsConnectionTest.HandshakeEncryptAndDecryptRoundTrip` | Memory BIO 握手、解密请求、加密响应可闭环。 |
| `HttpServerTest.SendTlsResponseEncryptsHeaderAndBody` | HTTP 响应经 TLS 加密后可由客户端解密。 |
| `HttpServerTest.TlsReadFailureClosesConnection` | TLS 读取失败会关闭并清理连接。 |

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 两级资源 | `SSL_CTX` 服务端共享，`SSL` 每连接独有。 |
| Memory BIO | OpenSSL 处理内存数据，Reactor 继续唯一管理 Socket。 |
| 握手回包 | 读操作也可能产生待发密文，必须反向发送。 |
| 非阻塞语义 | `WANT_READ/WANT_WRITE` 表示等待，不等于 TLS 失败。 |
| 失败收口 | TLS 会话不可用时释放 `SSL`，由 HttpServer 关闭 TCP 连接。 |

# 面试核心问答

## Q1：为什么要分成 `TlsConfig` 和 `TlsConnection`？

证书和协议策略属于服务器共享资源，而握手进度与会话密钥属于单连接资源。分离后避免每个连接重复加载证书，也使资源所有权清晰。

## Q2：为什么不用 `SSL_set_fd()`？

Socket 已由 Reactor 的 `EventLoop`、`TcpConnection` 和 Buffer 管理。Memory BIO 让 OpenSSL 只进行内存转换，避免绕开非阻塞事件循环或出现双重 I/O 管理。

## Q3：为什么读取 TLS 数据时还会有“待发送密文”？

TLS 握手是双向协议。客户端发来 ClientHello 后，服务端处理输入会生成 ServerHello 等握手消息，必须从写 BIO 取出并立刻发送。

## Q4：`WANT_READ` 与 TLS 错误有什么区别？

`WANT_READ` 表示当前密文不足，连接状态仍有效；真正错误会使 `TlsConnection` 释放 `SSL` 并返回 `Error`，上层关闭连接。

相关主题：[HttpConnection 设计](<HttpConnection 设计：粘包、HTTP Pipeline 与协议字节转换.md>)、[HttpServer 设计](<HttpServer 设计：协议编排、连接状态与生命周期安全.md>)。
