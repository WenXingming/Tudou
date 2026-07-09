# Tudou RPC 协议分帧与拆包粘包设计文档

在基于 TCP 的网络通信中，TCP 是一种**面向字节流**的无保护边界协议。多个网络包的字节流在传输时可能在接收端“粘在一起”（粘包），或者一个包被拆分多次送达（半包）。

Tudou RPC 框架支持两种截然不同的协议格式（强类型二进制 RPC 协议与弱类型文本 JSON-RPC 协议），本篇文档详细记录了这两种协议在服务端是如何优雅地解决粘包和半包问题的。

---

## 1. Binary RPC（二进制协议）的分帧与拆包设计

二进制 RPC 采用 **固定头部长度 + Payload 长度字段（Length-based Framing）** 的方式来进行精确分帧。

### A. 二进制帧布局
每个二进制数据包都有一个固定的 **20 字节包头**，结构如下：
- `magic` (2 字节): 固定为魔数 `0x5444` ("TD")，用以快速校验协议包的合法性。
- `version` (1 字节): 协议版本。
- `type` (1 字节): 请求/响应/心跳类型。
- `sequenceId` (8 字节): 会话 ID。
- `metaLen` (4 字节): 后续 `Meta` 数据流的长度。
- `bodyLen` (4 字节): 后续 `Body` 业务数据流的长度。

### B. 服务端应用层半包缓冲区
在 [BinaryRpcServer.cpp](file:///home/wxm/Tudou/src/tudou/rpc/binary/BinaryRpcServer.cpp) 中，服务端使用了一个关联容器维护每条连接的输入缓冲区：
```cpp
std::unordered_map<TcpConnection*, std::string> connectionBuffers_;
```
当连接关闭（`on_close`）时，会将该连接对应的缓存擦除，防止内存泄漏。

### C. 拆包状态机控制（BinaryRpcCodec）
当 `on_message` 回调触发时，新接收的字节流追加进 `connectionBuffers_`，并装载入临时的解包 `Buffer` 中，通过死循环执行解包：

```cpp
BinaryRpcCodec::DecodeResult result = BinaryRpcCodec::decode(&buf, header, metaRaw, bodyRaw);
```

`BinaryRpcCodec::decode` 解包状态机内部的逻辑处理如下：

```
                              ┌────────────────────────┐
                              │ Buffer.readable_bytes()│
                              └───────────┬────────────┘
                                          │
                                   ＜ 20 字节包头?
                                     /          \
                                 (是)            (否)
                                 /                  \
                     ┌──────────▼────────┐     读取 20 字节 Header，
                     │返回 HalfPack (半包)│     解析出 metaLen 和 bodyLen
                     └───────────────────┘            │
                                                      │
                                           ＜ (20 + metaLen + bodyLen)?
                                             /                     \
                                         (是)                       (否)
                                         /                             \
                             ┌──────────▼────────┐            ┌─────────▼─────────┐
                             │返回 HalfPack (半包)│            │1. 提取 Meta 与 Body│
                             └───────────────────┘            │2. 推进 Buffer 读指针│
                                                              │3. 返回 Success      │
                                                              └───────────────────┘
```

1. **魔数校验 (`Error`)**：若首 2 字节魔数不等于 `0x5444`，说明该连接传输了非法流或数据损坏。此时直接返回 `Error`，服务端收到后会**立刻强制关闭连接 (`conn->force_close()`)**，防止内存被非法流无限制打满。
2. **包头未就绪 (`HalfPack`)**：若当前可读字节数小于包头长度（20 字节），不移动读指针（回滚），返回 `HalfPack` 退出解包循环，等待下一次数据到来。
3. **负载未就绪 (`HalfPack`)**：成功读取包头后，校验当前可读总字节数是否小于 `20 + metaLen + bodyLen`。如果是，说明后半段数据（Payload）尚在网络传输中。此时**不推进读指针（回滚）**，返回 `HalfPack` 并退出循环。
4. **包解析成功 (`Success`)**：当缓冲区拥有完整数据帧时，提取出 `Meta` 和 `Body` 数据，**安全向前推进读指针**，返回 `Success`。服务端开始调用反射派发业务，并继续循环检查下一个粘在后面的包。

最后，未消费的剩余字节（半包部分）被重新存回该连接的 `connectionBuffers_` 缓存中。

---

## 2. JSON-RPC（文本协议）的分帧与拆包设计

与二进制协议不同，JSON-RPC 采用的是**定界符分包（Delimiter-based Framing）**。

### A. 文本帧布局
JSON-RPC 2.0 请求是纯文本 JSON 串。Tudou 框架规定：**每个完整的 JSON-RPC 请求必须以换行符 `\n` 作为结尾**。
例如：
```json
{"jsonrpc": "2.0", "method": "add", "params": [1, 2], "id": 1}\n
```

### B. 换行拆包处理
在 [JsonRpcServer.cpp](file:///home/wxm/Tudou/src/tudou/rpc/json/JsonRpcServer.cpp) 中，同样对每条连接维护了 `std::string` 接收缓存：
1. **追加缓冲**：收到新数据时，追加至该连接对应的 `connBuf` 尾部。
2. **检索换行符**：执行死循环，在 `connBuf` 中检索换行符的位置：
   ```cpp
   size_t pos = connBuf.find('\n');
   ```
3. **半包判定**：如果找不到 `\n`，说明当前行请求数据尚未完全接收完毕（半包），直接退出解包循环，等待后续套接字数据。
4. **粘包解析**：若找到 `\n`，则提取 `0` 到 `pos` 之间的字符串作为完整的 JSON 请求交予路由器处理，并使用 `connBuf.erase(0, pos + 1)` 抹去已被消费的数据。之后继续循环处理剩余部分（处理可能粘在后面的下一个 JSON 包）。

---

## 3. 大端/主机字节序转换机制

在网络物理媒介上传输整型字段（如 Length，SequenceID）时，由于不同 CPU 架构存在大端（Big-Endian）与小端（Little-Endian）的差异，直接传输内存字节会导致乱序。

Tudou RPC 在 [BinaryRpcCodec.cpp](file:///home/wxm/Tudou/src/tudou/rpc/binary/BinaryRpcCodec.cpp) 中采用了业界统一的**大端字节序（网络序）**作为标准格式：
- **发送编码时 (`encode`)**：使用 `htobe16` (16位)、`htobe32` (32位) 和 `htobe64` (64位) 将主机字节序（通常是 Little-Endian）的魔数、长度及序列 ID 转换为网络大端序，写入网络缓冲区。
- **接收解码时 (`decode`)**：使用 `be16toh`、`be32toh` 和 `be64toh` 将读取的网络大端序还原回 CPU 能够正常计算的主机字节序。
- 这确保了 Tudou 服务端与不同硬件架构上的客户端（如 ARM 移动端、x86 服务器等）进行数据交互时的协议正确性。
