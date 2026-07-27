# Tudou 零拷贝传输设计：从明文 HTTP 到 HTTPS kTLS

Tudou 对文件响应采用两条路径：明文 HTTP 使用 `sendfile`，HTTPS 在支持 Linux kTLS 时把 TLS 记录层卸载到内核后继续使用 `sendfile`，不支持时回退到用户态 Memory BIO。

# Situation — 情境

传统文件发送通常是 `read(file) → write(socket)`，文件内容需要经过用户态缓冲区，带来额外的 CPU 拷贝。

明文 HTTP 可以直接使用 Linux `sendfile`。但 HTTPS 需要先加密文件内容，普通 `sendfile` 无法直接处理明文文件和 TLS 密文之间的转换。

# Task — 任务

需要同时满足：

1. 明文文件响应尽量减少用户态拷贝；
2. HTTP 头部必须先于文件内容发送；
3. 非阻塞 socket 遇到 `EAGAIN` 时可以继续传输；
4. HTTPS 支持时利用 kTLS 继续走内核发送；
5. kTLS 不可用或卸载失败时自动回退，不影响连接可用性。

# Action — 设计与实现

### 明文 HTTP：sendfile

`sendfile(out_fd, in_fd, offset, count)` 让内核直接把文件数据发送到 socket，绕过用户态文件缓冲区。

非阻塞发送需要先处理 HTTP 头部：

```text
send_file_with_header
  → 先发送 HTTP Header
  → Header 未发完：等待可写事件
  → Header 发完：调用 sendfile
  → EAGAIN：保存 offset，等待下次可写事件
  → 文件发送完成：关闭文件 fd
```

业务层只需要提供文件 fd 和大小：

```cpp
server.add_get_route("/download", [](const HttpRequest&, HttpResponse& resp) {
    int fd = ::open("largefile.zip", O_RDONLY | O_CLOEXEC);
    resp.set_status(200, "OK");
    resp.set_file_body(std::make_shared<ScopedFd>(fd), fileSize);
});
```

### HTTPS：Linux kTLS

kTLS 把 TLS 记录层的对称加密下沉到内核，握手和密钥协商仍由 OpenSSL 在用户态完成。握手成功后：

```text
Memory BIO 完成握手
  → setsockopt(fd, TCP_ULP, "tls")
  → 向内核注入会话密钥
  → sendfile 发送文件明文
  → 内核自动加密为 TLS 记录并通过 TCP 发送
```

这样 HTTPS 文件发送可以复用明文的文件传输路径。

### 能力探测与回退

kTLS 依赖 Linux 内核、OpenSSL 版本和系统权限。`TlsProbe` 在编译期和运行期检查支持情况。卸载失败时不把连接标记为 kTLS，而是继续使用 Memory BIO：

```cpp
if (tlsConnection->enable_ktls_offload(conn->get_fd())) {
    state->isKtlsOffloaded = true;
} else {
    state->tlsMode = TlsMode::MemoryBio;
}
```

### 响应路径分流

```text
HttpServer::send_http_response
  ├─ 明文 / kTLS
  │    ├─ 普通响应 → TcpConnection::send
  │    └─ 文件响应 → sendfile
  └─ Memory BIO
       ├─ SSL_write 加密
       └─ TcpConnection::send 发送密文
```

# Result — 结果

- 明文文件响应使用内核 `sendfile`；
- 支持 kTLS 时，HTTPS 文件也可以复用内核文件发送；
- 非阻塞发送保存 offset，能够在多次可写事件中完成；
- kTLS 不可用时自动回退到 Memory BIO；
- 业务层不需要了解具体传输模式。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 明文零拷贝 | 使用 `sendfile` 减少文件到用户态的拷贝。 |
| kTLS | 握手在用户态，TLS 记录加密在内核。 |
| 非阻塞 | Header、文件 offset 和 `EAGAIN` 状态由发送流程保存。 |
| 回退 | kTLS 失败时继续使用 Memory BIO，不中断 HTTPS。 |

# 面试核心问答总结

## Q1：为什么普通 HTTPS 不能直接 sendfile？

文件是明文，而 HTTPS 需要发送 TLS 密文。普通 `sendfile` 不会执行 TLS 加密。

## Q2：kTLS 做了什么？

它把握手后的 TLS 记录层对称加密下沉到内核，使内核可以在 sendfile 路径中直接加密文件数据。

## Q3：kTLS 失败怎么办？

不把连接切换到 kTLS，继续使用用户态 Memory BIO 加密发送，功能正确性优先于性能优化。
