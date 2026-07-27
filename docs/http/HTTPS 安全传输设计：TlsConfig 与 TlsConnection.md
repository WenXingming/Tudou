# HTTPS 安全传输设计：TlsConfig 与 TlsConnection 的职责分工与协作

Tudou 将 OpenSSL 封装为两个组件：`TlsConfig` 保存服务器级 TLS 配置，`TlsConnection` 保存单条连接的握手和加解密状态。二者通过 Memory BIO 接入非阻塞 Reactor。

# Situation — 情境

HTTPS 需要完成证书配置、TLS 握手、数据加解密和非阻塞发送。如果让每条连接直接管理证书和全局策略，会造成重复初始化；如果让 OpenSSL 直接接管 socket，又会绕过 EventLoop 的 I/O 管理。

# Task — 任务

设计需要满足：

1. 全局 TLS 配置只初始化一次并可被多个连接使用；
2. 每条连接独立保存自己的握手和加解密状态；
3. OpenSSL 不直接阻塞读写 socket；
4. 密文和明文在 TLS 层与 HTTP 层之间清晰传递；
5. 最低协议版本等安全策略集中配置。

# Action — 设计与实现

### TlsConfig 与 TlsConnection 的职责

| 组件 | 主要职责 | 生命周期 |
| :--- | :--- | :--- |
| `TlsConfig` | 加载证书和私钥、配置协议策略、创建 `SSL*` | 服务器级 |
| `TlsConnection` | 保存握手状态、加密发送数据、解密接收数据 | 连接级 |

`TlsConfig` 内部持有 `SSL_CTX`，初始化完成后主要以只读方式被多个 I/O 线程使用；`TlsConnection` 持有当前连接的 `SSL` 和两个 Memory BIO，只能由所属 EventLoop 操作。

### Memory BIO 接入 Reactor

OpenSSL 不直接绑定 socket，而是使用两个内存 BIO：

- `rbio_`：保存从 socket 读到的 TLS 密文，作为解密输入；
- `wbio_`：保存 OpenSSL 生成的 TLS 密文，等待 socket 发送。

```text
socket 密文
  → rbio_
  → SSL_read / SSL_do_handshake
  → HTTP 明文

HTTP 明文
  → SSL_write
  → wbio_
  → socket 密文
```

### 接收和解密

1. EventLoop 收到 socket 可读事件；
2. `TcpConnection::receive()` 读出密文；
3. 密文写入 `rbio_`；
4. 握手阶段调用 `SSL_do_handshake`；
5. 握手完成后调用 `SSL_read`，得到 HTTP 明文；
6. 明文交给 HTTP 解析器。

如果输入不足，OpenSSL 返回 `SSL_ERROR_WANT_READ`，连接保留当前状态，等待下一次可读事件。

### 加密和发送

1. HTTP 层生成响应明文；
2. 调用 `SSL_write` 写入 OpenSSL；
3. OpenSSL 将密文写入 `wbio_`；
4. 通过 `BIO_read` 取出密文；
5. 交给 `TcpConnection::send()`，由非阻塞 socket 发送。

# Result — 结果

- 全局配置与连接状态分离；
- OpenSSL 只处理内存中的 TLS 数据，不直接控制网络 I/O；
- 握手、加密和解密可以融入现有 EventLoop；
- HTTP 层只看到明文，TCP 层只负责字节传输。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 配置与会话分离 | `TlsConfig` 管全局策略，`TlsConnection` 管单连接状态。 |
| Memory BIO | 将 OpenSSL 变成内存过滤器，网络 I/O 仍由 Reactor 控制。 |
| 非阻塞协作 | `WANT_READ` / `WANT_WRITE` 时保留状态，等待下一次事件。 |
| 分层边界 | TLS 处理密文转换，HTTP 处理解密后的协议内容。 |

# 面试核心问答总结

## Q1：为什么需要 TlsConfig 和 TlsConnection 两个类？

证书和协议策略是服务器级共享配置，握手和加解密状态是连接级数据。分开后可以避免重复初始化，也能明确生命周期。

## Q2：为什么不用 SSL_set_fd 直接让 OpenSSL 操作 socket？

Tudou 的 socket 由 EventLoop 统一管理。Memory BIO 让 OpenSSL 只处理内存数据，避免绕过 Reactor 或产生阻塞 I/O。

## Q3：WANT_READ 代表什么？

表示当前输入不足以继续握手或解密。连接不能被当成失败，应保留状态并等待更多网络数据。
