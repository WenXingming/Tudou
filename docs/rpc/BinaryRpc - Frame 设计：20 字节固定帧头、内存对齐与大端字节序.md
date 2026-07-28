# BinaryRpc - Frame 设计：20 字节固定帧头、内存对齐与大端字节序

`binary::Frame` 是二进制 RPC 的完整协议对象，按照线上顺序由 `FrameHeader + head + body` 组成。`FrameHeader` 解决分帧和请求关联，`head` 描述调用目标，`body` 保存业务 Protobuf 消息。

# Situation — 情境

TCP 只提供连续字节流，没有 RPC 消息边界。一次 read 可能只得到半个请求，也可能同时得到多个请求。客户端还会在一条 TCP 长连接上并发发起调用，响应到达顺序不一定与发送顺序相同。

因此协议必须回答：

1. 当前字节是不是合法的 Tudou RPC 帧；
2. 一条完整帧应该读取多少字节；
3. 当前是请求还是响应；
4. 响应属于之前的哪次调用；
5. 请求应该交给哪个 Protobuf Service 和 Method。

# Task — 任务

设计一个容易检查和解析的帧格式，同时保证：

- 固定帧头没有编译器填充；
- 不同主机使用相同的线上字节序；
- 半包到达时可以等待，粘包到达时可以连续拆帧；
- 单连接上的并发请求能够通过序列号匹配响应；
- 协议路由信息与具体业务消息保持分离；
- 非法长度不能导致无界内存占用。

# Action — 设计与实现

## 完整帧布局

```text
+---------------------- 20 字节 ----------------------+
|                    FrameHeader                      |
+-------------------+---------------------------------+
| headLength 字节    | bodyLength 字节                 |
| head              | body                            |
+-------------------+---------------------------------+
```

线上完整帧大小为：

```text
20 + headLength + bodyLength
```

## FrameHeader：固定 20 字节帧头

```text
+--------+---------+------+------------+------------+------------+
| magic  | version | type | sequenceId | headLength | bodyLength |
| 2 byte | 1 byte  | 1 B  | 8 bytes    | 4 bytes    | 4 bytes    |
+--------+---------+------+------------+------------+------------+
```

| 字段 | 大小 | 当前值或语义 | 解决的问题 |
| :--- | ---: | :--- | :--- |
| `magic` | 2 B | `0x5444`，即 `TD` | 快速拒绝非 Tudou RPC 数据 |
| `version` | 1 B | 当前为 `1` | 防止不同协议版本被误解析 |
| `type` | 1 B | `Request` 或 `Response` | 区分请求帧和响应帧 |
| `sequenceId` | 8 B | 每次调用的序列号 | 将乱序响应关联到原请求 |
| `headLength` | 4 B | `head` 的字节数 | 确定调用头边界 |
| `bodyLength` | 4 B | `body` 的字节数 | 确定业务消息边界 |

字段大小之和严格为：

```text
2 + 1 + 1 + 8 + 4 + 4 = 20 字节
```

`#pragma pack(push, 1)` 禁止编译器在字段之间插入填充字节，`static_assert(kHeaderSize == 20)` 则在编译期验证布局。协议格式直接定义在 `Frame.h` 中，打开头文件即可看到真实帧头。

## sequenceId：单连接并发请求关联

同一条连接可以连续发送多个请求：

```text
Request(seq=1) ───────────────┐
Request(seq=2) ────────┐      │
                       ↓      ↓
Response(seq=2)   Response(seq=1)
```

服务端把请求的 `sequenceId` 原样写入响应。客户端不依赖响应顺序，而是使用该序列号找到对应的等待线程或协程。

## head：调用目标

请求帧的 `head` 是 `CallHead` 的 Protobuf 序列化结果：

```protobuf
message CallHead {
    string service_name = 1;
    string method_name = 2;
}
```

它回答“调用哪个服务的哪个方法”。例如：

```text
service_name = tudou.rpc.binary.EchoService
method_name  = Echo
```

响应已经可以通过 `sequenceId` 找到原调用，因此当前响应帧的 `head` 为空。

## body：业务 Protobuf 消息

`body` 不属于固定协议结构，它由目标方法的 Protobuf 类型决定：

```text
请求帧 body = Request Message 的序列化字节
响应帧 body = Response Message 的序列化字节
```

Frame 和 FrameCodec 不理解具体业务类型。只有 Router 根据 service/method 的 Descriptor 创建正确的 Message 并完成反序列化。

## Frame：完整协议对象

```cpp
struct Frame {
    FrameHeader header;
    std::string head;
    std::string body;
};
```

构造 Frame 时会同步填写：

```cpp
header.type = frameType;
header.sequenceId = frameSequenceId;
header.headLength = head.size();
header.bodyLength = body.size();
```

FrameCodec 编码前还会再次检查两个长度字段与字符串是否一致，拒绝发送自相矛盾的完整帧。

## C++ 对象布局不等于线上字节布局

Frame 虽然完整表达了协议，但不能直接把整个 C++ 对象 `memcpy` 到 Socket。`std::string` 内部包含指针、长度和容量等管理状态，并不是内联存放在 Frame 后面的 payload。

FrameCodec 实际发送：

```text
FrameHeader 的 20 个字节
+ head.data() 指向的 headLength 个字节
+ body.data() 指向的 bodyLength 个字节
```

对象中的 FrameHeader 使用主机字节序；编码时 FrameCodec 复制帧头并将多字节整数转换为网络大端序，解码时再转换回主机字节序。这样上层代码可以直接使用正常整数，线上表示仍然跨主机一致。

## 帧大小限制

完整帧最大为 64 MiB。FrameCodec 在等待整个 payload 之前先验证：

```text
headLength + bodyLength <= kMaxFrameSize - kHeaderSize
```

这能拒绝伪造的超大长度，避免连接长期等待不存在的数据或诱导缓冲区无界增长。

# Result — 结果

- 固定帧头为 TCP 字节流提供明确的 RPC 消息边界；
- 半包可以等待后续数据，粘包可以按长度连续拆分；
- `sequenceId` 支持单 TCP 长连接上的并发请求和乱序响应；
- `CallHead` 提供动态 Protobuf 路由信息，body 保持业务无关；
- 1 字节对齐和大端序保证线上布局稳定；
- 长度一致性校验和 64 MiB 上限阻止非法帧消耗过多资源。

# 面试核心问答

## Q1：为什么 TCP RPC 需要自己设计帧头？

TCP 没有消息边界。固定帧头中的两个长度字段让接收方知道一条 RPC 一共有多少字节，从而正确处理半包和粘包。

## Q2：为什么需要 sequenceId？

一条 TCP 连接上可以同时存在多个未完成请求，响应也可能乱序到达。sequenceId 将每个响应精确关联到原请求，使单连接多路复用成立。

## Q3：为什么把 service/method 放在单独的 head 中？

业务 Protobuf body 本身不能说明应该交给哪个 Service/Method。CallHead 补充路由信息，同时让业务消息保持独立。

## Q4：Frame 已经包含 header、head、body，为什么不能直接发送 sizeof(Frame)？

因为两个 `std::string` 保存的是 C++ 容器管理状态，而不是连续跟随在 FrameHeader 后的实际字符。FrameCodec 必须分别写入固定帧头和两个字符串的数据。

## Q5：为什么使用网络大端序？

不同 CPU 的主机字节序可能不同。统一使用网络大端序，可以让同一份协议字节在不同机器上得到一致解释。

# 面试表达

> Tudou Binary RPC 在 TCP 之上定义了 `FrameHeader + CallHead + Body` 的完整帧。20 字节固定帧头用 magic/version 校验协议，用两个长度字段解决半包粘包，用 sequenceId 支持单连接并发请求关联；CallHead 使用 Protobuf 保存 service/method，Body 保存具体业务消息。帧头采用 1 字节对齐并统一转换为网络大端序，同时限制完整帧最大 64 MiB。
