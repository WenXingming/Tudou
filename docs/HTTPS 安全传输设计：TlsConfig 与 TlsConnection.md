# HTTPS 安全传输设计：TlsConfig 与 TlsConnection 的职责分工与协作

在 Tudou 的 HTTP 模块中，对 HTTPS（TLS 安全传输）的支持是通过对 OpenSSL 进行封装实现的。这里引入了两个核心组件：**[TlsConfig](file:///home/wxm/Tudou/src/tudou/http/TlsConfig.h)** 与 **[TlsConnection](file:///home/wxm/Tudou/src/tudou/http/TlsConnection.h)**。它们分工协作，分别承担了“全局配置工厂”与“单连接会话”的角色。

---

## 0. TLS 与 SSL 的联系与区别

虽然在日常交流中人们经常混用 "SSL" 与 "TLS"（例如习惯称呼 "SSL 证书"），但在技术和安全标准上，它们有着明确的迭代关系：

1. **SSL (Secure Sockets Layer，安全套接层)**：
   * 由网景（Netscape）公司在 1990 年代开发（SSL 2.0 / SSL 3.0）。
   * 随着时代发展，其加密算法已被证实存在严重漏洞（如 POODLE 攻击）。
   * 2015 年，互联网工程任务组（IETF）已**正式废弃所有版本的 SSL 协议**，目前在安全通信中被视为不安全协议。
2. **TLS (Transport Layer Security，传输层安全)**：
   * 是 IETF 在标准化 SSL 3.0 基础上推出的升级版协议（TLS 1.0 实际上即为 SSL 3.1）。
   * 经历 TLS 1.1、1.2 至今演进到现代的 TLS 1.3，安全性与握手效率有了质的飞跃。
   * Tudou 的 HTTPS 实现**在配置层强制规定最低只允许 TLS 1.2 及以上协议运行**，完全摒弃了过时的 SSL 协议。

由于 OpenSSL 的 C API 命名中仍带有大量 `SSL_` 历史痕迹，本网络库选择在业务层将概念彻底理顺，统一采用现代化的 **`Tls`** 前缀进行命名。

---

## 1. TlsConfig 与 TlsConnection 的职责对比

在 TLS 的生命周期管理中，全局的安全策略配置与单条连接的握手状态是天然隔离的：

| 维度 | TlsConfig | TlsConnection |
| :--- | :--- | :--- |
| **底层封装类型** | OpenSSL `SSL_CTX` 结构体指针 | OpenSSL `SSL` 结构体指针 + 双 `BIO` 内存缓冲 |
| **生命周期** | **全局唯一**。随服务器 `HttpServer` 启动而初始化，随服务器关闭而销毁。 | **连接级唯一**。随客户端 TCP 连接建立而创建，随连接断开而销毁。 |
| **命名定位** | **`Config`（配置）**：直观体现其作为“静态、只读、全局共享配置仓库”的静态认知属性。 | **`Connection`（连接）**：代表“动态、单条 live 链路专属”的活动属性。 |
| **核心职责** | 1. 加载服务器公钥证书链与私钥。<br>2. 校验证书与私钥的匹配性。<br>3. 配置全局安全策略（如限制最低协议版本为 TLS 1.2）。 | 1. 维护当前连接的 TLS 握手状态机。<br>2. 将明文响应加密为待发送的网络密文。<br>3. 将接收到的网络密文解密为应用层明文。 |
| **线程安全** | **多线程安全**。`SSL_CTX` 只有在初始化时写入，在运行时为只读状态。各 IO 线程可以并发安全地用它来创建 `SSL` 结构体。 | **非线程安全**。由于保存了单条连接的握手进度和加解密状态，它必须完全被约束在对应的 `EventLoop` 线程（IO 线程）中操作。 |

---

## 2. 经典设计模式：工厂与产品

在 `HttpServer` 中，`TlsConfig` 和 `TlsConnection` 的关系是典型的**工厂模式**：
* **`TlsConfig` 是配置工厂**。它只负责保存通用的、不随连接变化的配置（证书、密钥、协议政策），并提供 `create_ssl()` 接口作为产品发生器。
* **`TlsConnection` 是会话产品**。每当有新客户端连接接入（`on_connect`），`HttpServer` 会调用 `tlsConfig_->create_ssl()` 生产出一个专属的 `SSL` 句柄，然后交由对应的 `TlsConnection` 接管。

```text
       HttpServer (持有全局 TlsConfig)
             │
             │ 1. 接收到新客户端连接 (TcpConnectionPtr)
             ▼
       TlsConfig::create_ssl() ───(派生)───►  OpenSSL SSL* (含当前连接的安全配置)
                                                 │
                                                 │ 2. 实例化并接管
                                                 ▼
                                           TlsConnection (绑定专属的 rbio/wbio)
```

---

## 3. 非阻塞网络模型中的 Memory BIO 协作机制

在传统的阻塞式网络编程中，OpenSSL 的 `SSL_read` 和 `SSL_write` 会直接绑定套接字 fd（用 `SSL_set_fd`），并接管网络 I/O 动作。

但是在非阻塞的 Reactor 模型中，**网络 I/O 必须统一由 EventLoop（Poller）管理**。我们不能让 OpenSSL 自行在底层调用 `read` 或 `write` 导致线程阻塞。

为了解决这一矛盾，Tudou 在 `TlsConnection` 中引入了 **Memory BIO（内存输入输出管道）** 机制：
* **读 Memory BIO (`rbio_`)**：作为解密的输入端。
* **写 Memory BIO (`wbio_`)**：作为加密的输出端。

### 3.1 接收并解密数据流 (Read Flow)
当套接字触发可读事件时：
1. `HttpServer::on_message` 通过 `TcpConnection::receive()` 获取网络上的密文数据。
2. 调用 `TlsConnection::read_plaintext`，将密文通过 `BIO_write` 写入 `rbio_` 中。
3. 如果当前处于握手期，调用 `SSL_do_handshake`，OpenSSL 会自动从 `rbio_` 读取数据推进握手，并将握手响应密文输出到 `wbio_`。
4. 如果握手已建立，调用 `SSL_read`，OpenSSL 从 `rbio_` 读取并解密密文，还原出应用层的 HTTP 明文。

### 3.2 加密并发送数据流 (Write Flow)
当应用层需要发送 HTTP 响应时：
1. `HttpServer::send_response` 传入 HTTP 响应明文。
2. 调用 `TlsConnection::write_plaintext`，通过 `SSL_write` 将明文喂给 OpenSSL。
3. OpenSSL 完成加密，并将生成的 TLS 密文写入到 `wbio_` 中。
4. 调用 `BIO_read` 从 `wbio_` 抽干密文，交回 `TcpConnection::send()` 通过非阻塞套接字发送给客户端。

---

## 4. 总结

`TlsConfig` 与 `TlsConnection` 的清晰分离使得安全传输层逻辑非常扁平：
* **全局与局部的分离**：全局配置归 `TlsConfig`，单条连接的状态归 `TlsConnection`，符合单一职责原则。
* **I/O 与加解密逻辑的解耦**：通过 Memory BIO，OpenSSL 仅作为“内存加解密过滤器”，而实际的网络读写权依然百分之百被 Reactor 控制，完美契合非阻塞事件驱动模型。
