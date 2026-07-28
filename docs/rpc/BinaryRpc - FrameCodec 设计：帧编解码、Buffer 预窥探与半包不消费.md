# BinaryRpc - FrameCodec 设计：帧编解码、Buffer 预窥探与半包不消费

`binary::FrameCodec` 负责 `Frame` 与网络字节之间的双向转换。它不读取 Socket、不保存连接状态，也不独自解决粘包；每条连接的残留字节由 `binary::Connection` 保存。

# Situation — 情境

TCP 是字节流协议，没有消息边界。一次读取可能只得到部分帧，也可能同时得到多个帧。二进制 RPC 因此需要根据固定帧头中的长度字段恢复完整 `Frame`。

解析还必须面对两个问题：

- 数据不足时不能提前消费 Buffer，否则下一批字节到达后无法重新读取完整帧头；
- 对端提供的帧头不可信，必须在等待和分配大块数据前校验协议字段及长度上限。

# Task — 任务

FrameCodec 需要完成两个方向的转换：

```text
encode:     Frame  -> 20 字节大端帧头 + head + body
try_decode: Buffer -> 一个完整 Frame
```

它同时保证：

- 编码时拒绝内部字段互相矛盾的 `Frame`；
- 解码时区分完整帧、半包和非法帧；
- 半包和非法帧都不改变 Buffer；
- 一次解码只消费一个完整帧。

# Action — 设计

## 1. 编码前检查 Frame 不变量

`Frame` 的帧头公开保存 `headLength` 和 `bodyLength`，因此调用方可能修改字符串后忘记同步长度。FrameCodec 在发送前再次检查：

```cpp
const size_t payloadSize = frame.head.size() + frame.body.size();
if (payloadSize > kMaxFrameSize - kHeaderSize) {
    throw std::length_error("FrameCodec: Frame is too large");
}

if (frame.header.magic != kMagic
    || frame.header.version != kVersion
    || !is_valid_type(frame.header.type)
    || frame.header.headLength != frame.head.size()
    || frame.header.bodyLength != frame.body.size()) {
    throw std::invalid_argument("FrameCodec: Frame header does not match payload");
}
```

这道检查避免把一个“帧头说一套、实际内容又是另一套”的帧发送到网络。

## 2. 只转换帧头副本的字节序

程序中的 `FrameHeader` 使用主机字节序，线上帧头使用网络大端序。编码时复制帧头并转换副本，不修改调用方传入的 `Frame`：

```cpp
FrameHeader header = frame.header;
header.magic = htons(header.magic);
header.sequenceId = htobe64(header.sequenceId);
header.headLength = htonl(header.headLength);
header.bodyLength = htonl(header.bodyLength);

std::string bytes(reinterpret_cast<const char*>(&header), kHeaderSize);
bytes.append(frame.head);
bytes.append(frame.body);
```

完整的 20 字节帧头布局由 [BinaryRpc - Frame 设计](<BinaryRpc - Frame 设计：20 字节固定帧头、内存对齐与大端字节序.md>) 说明。

## 3. 解码使用三态结果

```cpp
enum class DecodeResult {
    Complete,
    NeedMoreData,
    Invalid
};
```

三种结果会让连接采取不同动作：

| 结果 | Buffer 是否被消费 | 调用方行为 |
|---|---:|---|
| `Complete` | 消费一个完整帧 | 处理该帧并继续尝试解码 |
| `NeedMoreData` | 不消费 | 保留连接，等待后续 TCP 字节 |
| `Invalid` | 不消费 | 认为协议流已损坏并关闭连接 |

不能用 `bool` 合并后两种结果，因为“等待更多数据”和“立即关闭连接”的语义完全不同。

## 4. 先预窥探完整帧，再统一消费

`try_decode()` 的判断顺序就是解码流程：

```text
帧头是否达到 20 字节？
  -> 否：NeedMoreData

复制帧头但不移动 Buffer 读索引
  -> 校验 magic、version、type
  -> 转换并校验 headLength、bodyLength 和最大帧长

完整帧是否已经到齐？
  -> 否：NeedMoreData

一次消费帧头、head 和 body
  -> 构造 Frame
  -> Complete
```

核心代码只在确认完整帧已经到齐后才移动读索引：

```cpp
if (buffer.readable_bytes() < frameSize) {
    return DecodeResult::NeedMoreData;
}

buffer.advance_read_index(kHeaderSize);
std::string head = buffer.read_from_buffer(headLength);
std::string body = buffer.read_from_buffer(bodyLength);
frame = Frame(header.type, sequenceId, std::move(head), std::move(body));
```

因此半包不需要保存中间解析状态，也不需要回滚 Buffer。

## 5. FrameCodec 与 Connection 的边界

FrameCodec 每次只尝试提取一个帧。`binary::Connection` 才拥有跨多次 TCP 回调保留的 `inputBuffer_`，并循环调用 FrameCodec：

Connection 的连接级状态、线程归属和 Buffer 所有权详见 [BinaryRpc - Connection 设计](<BinaryRpc - Connection 设计：连接级 Buffer、半包保留与粘包拆分.md>)。

```cpp
inputBuffer_.write_to_buffer(data);

while (true) {
    Frame frame;
    const auto result = FrameCodec::try_decode(inputBuffer_, frame);
    if (result == FrameCodec::DecodeResult::Complete) {
        frames.push_back(std::move(frame));
        continue;
    }
    if (result == FrameCodec::DecodeResult::NeedMoreData) {
        return true;
    }
    return false;
}
```

由此形成清楚的职责边界：

```text
FrameCodec：判断并转换一个帧
Connection：保存一条连接的半包，并循环拆出粘在一起的多个帧
Server：收到非法流时记录错误并关闭 TCP 连接
```

FrameCodec 对非法数据不尝试扫描下一个 magic 重新同步。当前 RPC 连接采用“协议错误即关闭”的策略，所以额外的恢复状态机会增加复杂度，却没有实际收益。

# Result — 结果

- 编码和解码共享同一套帧格式与长度约束；
- 半包不消费 Buffer，不需要中间游标和回滚逻辑；
- 粘包由 Connection 的简单循环拆分，FrameCodec 保持无状态；
- 非法字段和超大帧会在读取完整 payload 前被拒绝；
- 客户端和服务端可以复用同一个 FrameCodec。

对应单元测试覆盖完整帧、半包、连续帧、非法帧头、超大帧以及帧头与 payload 不一致，共 6 个场景。

# 权衡

- `try_decode()` 使用输出参数返回 `Frame`，只有 `Complete` 时才写入它；这样避免为三态结果再引入 `variant` 或额外结果结构体。
- FrameCodec 是无状态静态工具类。它保留独立类型，是为了让 `Frame` 只描述数据，不依赖 TCP `Buffer` 或网络字节序细节。
- 当前最大帧长为 64 MiB，这是防御性上限，不代表服务端会为每个连接预分配 64 MiB。

# 面试核心问答

阅读问题后可以先不看答案，尝试沿着“谁保存状态、谁处理一个帧、何时消费 Buffer”三个线索自行回答。

### 1. FrameCodec 的核心职责是什么？

它负责 `Frame` 与网络字节之间的双向转换：`encode()` 把完整帧编码为大端网络字节，`try_decode()` 从 Buffer 中尝试提取一个完整帧。它不读取 Socket、不保存半包，也不执行 RPC 路由。

### 2. FrameCodec 和 Connection 分别解决半包、粘包中的哪一部分？

FrameCodec 通过“数据不足时不消费 Buffer”保证一个半包可以等待后续字节；Connection 持有连接级 `inputBuffer_`，并循环调用 FrameCodec，从一次收到的连续字节中提取多个完整帧。因此，半包缓存和粘包循环属于 Connection，单帧边界判断属于 FrameCodec。

### 3. 为什么 `try_decode()` 一次只解析一个帧？

一次只解析一个帧可以让接口和副作用保持明确：`Complete` 就恰好消费一个帧。至于 Buffer 中还有没有下一个帧，由 Connection 的循环决定。若 FrameCodec 一次返回多个帧，它就会同时承担连接级粘包处理，职责边界会变得模糊。

### 4. 为什么解码结果不能简化成 `bool`？

因为失败包含两种完全不同的控制流：

- `NeedMoreData` 不是错误，连接应继续保留并等待数据；
- `Invalid` 是协议错误，当前实现会关闭连接。

`Complete / NeedMoreData / Invalid` 三态直接表达了调用方下一步应该做什么。

### 5. 为什么半包时不能先消费已经读到的帧头？

如果提前移动 Buffer 读索引，就必须额外保存已解析的帧头和当前进度，或者在发现 payload 不完整后执行回滚。当前实现先预窥探帧头，等整个帧到齐后再一次消费，因此不需要中间状态和回滚逻辑。

### 6. 为什么 `Invalid` 时也不消费 Buffer？

FrameCodec 只报告“当前字节流不是合法帧”，不负责决定如何恢复。Server 收到失败后会关闭连接，整个连接 Buffer 随之销毁，因此没有必要为了丢弃非法字节而增加消费或重新同步逻辑。

### 7. 为什么要在完整 payload 到达前检查最大帧长？

长度字段来自不可信的网络输入。若先等待头部声明的全部 payload，恶意客户端可以填写极大的长度，让连接长期占用缓存和内存。FrameCodec 读取帧头后立即检查 64 MiB 上限，可以尽早拒绝异常帧。

### 8. 为什么编码时转换帧头副本，而不直接修改 `Frame`？

`Frame` 在程序内部始终使用方便计算的主机字节序，只有线上字节使用网络大端序。转换副本可以保持传入对象不变，避免一次编码后 `sequenceId` 和长度字段突然变成上层无法直接使用的网络字节序。

### 9. FrameCodec 没有成员变量，为什么不把方法放进 Frame？

是否拆分类取决于职责，不取决于有没有成员变量。`Frame` 只描述帧头、调用头和消息体；FrameCodec 负责 Buffer、网络字节序和协议校验。如果把解码放进 Frame，`Frame.h` 就要依赖 TCP 层的 `Buffer`，数据结构与传输细节会耦合。

### 10. 当前设计为什么不尝试从非法流中寻找下一个 magic？

重新同步需要扫描字节、处理伪 magic，并引入更多恢复状态。当前二进制 RPC 使用长连接，但协议错误意味着对端实现或数据已经不可信，直接关闭连接更简单可靠；客户端可以重新建立一条干净连接。

# 面试表达

> FrameCodec 负责 Frame 与网络字节的双向转换。解码时先预窥探固定 20 字节帧头，校验协议字段和长度，再确认完整帧是否到齐；只有完整时才一次消费 Buffer，所以半包无需回滚。它每次只解析一帧，Connection 保存连接级 Buffer 并循环调用它解决粘包。非法协议不做复杂的流重同步，而是直接关闭连接，使编解码器保持无状态、边界清晰。
