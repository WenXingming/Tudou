# BinaryRpc - Connection 设计：连接级 Buffer、半包保留与粘包拆分

`binary::Connection` 保存一条二进制 RPC 字节流的解析状态。它不拥有 Socket，也不管理 TCP 生命周期；它拥有连接级 `Buffer`，把多次收到的网络字节整理成按顺序排列的完整 `Frame`。

# Situation — 情境

TCP 只提供连续字节流，一次读取与一个 RPC 帧没有必然对应关系：

```text
半包：一次只收到一个 Frame 的前半部分
粘包：一次连续收到多个 Frame
组合：前一次留下半包，本次补齐后面又跟着其他 Frame
```

`FrameCodec::try_decode()` 只判断并提取一个帧，而且在数据不足时不消费 Buffer。还需要一个连接级对象保存剩余字节，并持续调用 FrameCodec，直到当前数据无法再组成完整帧。

# Task — 任务

Connection 需要保证：

- 每条 RPC 字节流拥有独立的半包缓存；
- 新收到的字节按顺序追加，不能覆盖旧半包；
- 一次调用输出当前能够组成的全部完整帧；
- 剩余半包保留到下一次调用；
- 非法帧立即报告失败，由调用方关闭连接；
- 不把 Socket、路由和业务回调混入分帧逻辑。

# Action — 设计

## 1. 每条连接独占一个 Buffer

Connection 只有一个成员：

```cpp
Buffer inputBuffer_;
```

这个成员虽然简单，却表达了关键不变量：

> 不同 TCP 连接的字节绝不能进入同一个解析 Buffer。

服务端为每个 `TcpConnection` 创建一个 Connection；阻塞客户端和协程客户端分别拥有自己的响应 Connection：

```text
Server:             TcpConnection* -> shared_ptr<Connection>
Channel:            Connection responseConnection_
CoroutineChannel:   Connection responseConnection_
```

Connection 不需要 mutex。服务端同一条 TCP 连接的消息回调由所属 EventLoop 串行执行；两个客户端也各自由唯一的接收线程或 EventLoop 使用自己的 Connection。

## 2. 追加字节后循环提取帧

`decode()` 直接展示完整流程：

```cpp
bool Connection::decode(const std::string& data, std::vector<Frame>& frames) {
    inputBuffer_.write_to_buffer(data);
    frames.clear();

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
}
```

控制流只有三条路径：

```text
Complete     -> 保存帧，继续解析后续字节
NeedMoreData -> 保留 Buffer 中的半包，正常返回
Invalid      -> 报告协议流非法
```

不需要额外记录“当前解析到帧头还是 body”。FrameCodec 在半包时不消费 Buffer，所以 Connection 只要保留 Buffer 本身。

## 3. 半包和粘包是同一个循环的两种结果

假设 `F1`、`F2` 是两个完整帧：

```text
第一次输入：F1 的前半部分
  -> NeedMoreData
  -> inputBuffer_ 保留半个 F1

第二次输入：F1 的后半部分 + F2
  -> 旧半包与新数据组成完整 F1
  -> Complete，输出 F1 并继续
  -> Complete，输出 F2 并继续
  -> Buffer 为空，NeedMoreData，正常结束
```

因此代码中不需要分别实现 `handle_half_packet()` 和 `handle_sticky_packet()`。半包由 Buffer 保留，粘包由 `while` 循环继续，两者自然统一。

## 4. `bool` 表达整条输入流是否仍然有效

FrameCodec 必须返回三态，因为调用方要区分一个帧是否完整。Connection 已经在内部消化了这三态，对上层只需要回答：

```text
true  -> 当前字节流合法；可能输出了零个或多个完整帧
false -> 遇到非法帧；当前连接应关闭
```

`NeedMoreData` 对 Connection 来说不是失败，而是长连接上的正常等待状态，因此与成功解析帧一起归为 `true`。这里继续使用 `bool` 比再定义一个只有“有效/非法”的枚举更简单。

## 5. 输出参数只表示本次产生的帧

`decode()` 在入口调用：

```cpp
frames.clear();
```

因此输出参数没有“调用前必须为空”的隐藏约束。函数返回时，`frames` 只保存本轮追加数据后新形成的完整帧，不会混入调用者上一次遗留的结果。

如果一批数据先包含合法帧、随后出现非法帧，Connection 会返回 `false`。当前 Server、Channel 和 CoroutineChannel 都会立即关闭或终止该连接，不处理这批输出，采用 fail-closed 策略。

## 6. 为什么拥有 Buffer，而不是引用 TcpConnection 的 Buffer

当前 TCP 层通过 `TcpConnection::receive()` 取出本轮网络字节，HTTP 和 RPC 协议层各自管理增量解析状态。让 Connection 直接拥有 Buffer 有三个好处：

- 不依赖 `TcpConnection` 内部 Buffer 的生命周期；
- 服务端、阻塞客户端和协程客户端可以复用同一实现；
- TCP 层不需要向应用协议暴露可修改的读索引。

代价是服务端收包路径多一次字节复制。当前没有性能数据证明这里是瓶颈，因此不为消除一次复制而扩大 TCP 层接口和生命周期约束。

# Result — 结果

- 一个值成员表达了全部连接级解析状态；
- 半包不需要状态机或回滚；
- 粘包只需要继续循环，不需要单独分支；
- 服务端与两种客户端共享相同的分帧行为；
- Connection 与 FrameCodec、TcpConnection、Router 的职责边界明确。

单元测试覆盖“跨两次输入补齐半包并继续拆出粘包”和“拒绝非法字节流”两条核心路径。

# 权衡

- Connection 名称表示“二进制 RPC 协议连接状态”，不是底层 Socket；它与 `HttpConnection` 的分层含义一致。
- 类只有一个 Buffer 和一个业务接口，但这个 Buffer 是跨调用状态，而且三个调用方都需要同一套循环，因此保留类比复制逻辑更简单。
- 当前接口接收 `std::string`，没有为了潜在的零拷贝收益改造整个 TCP 消息回调契约。

# 面试核心问答

### 1. Connection 与 TcpConnection 有什么区别？

`TcpConnection` 拥有 Socket、Channel 和 TCP 收发缓冲，负责网络生命周期；`binary::Connection` 只保存二进制 RPC 字节流的半包，并输出完整 Frame。二者是一对一关联，但职责处于不同协议层。

### 2. 为什么每条 TCP 连接必须有独立的 Connection？

因为半包属于特定字节流。若多个连接共用一个 Buffer，它们的字节会交叉，帧头长度和 payload 将失去对应关系。每连接一个 Connection 直接保证字节隔离。

### 3. Connection 怎样处理半包？

它先把新字节追加到 `inputBuffer_`。FrameCodec 发现完整帧尚未到齐时返回 `NeedMoreData` 且不消费 Buffer，Connection 随即正常返回；下一次数据会继续追加在旧半包之后。

### 4. Connection 怎样处理粘包？

每成功提取一个 Frame 就继续循环调用 FrameCodec，直到 Buffer 中不足以组成下一个完整帧。因此一次输入可以输出多个 Frame。

### 5. 为什么 Connection 不需要自己的解析状态机？

帧头提供完整长度，FrameCodec 又保证半包零消费。Connection 无需记住解析阶段，只要保存尚未消费的原始字节即可。

### 6. 为什么 `decode()` 返回 true 时可能一个 Frame 都没有？

因为收到半包是 TCP 长连接上的正常情况。`true` 表示“字节流目前合法”，不表示“一定解析出了帧”；是否有完整帧由输出容器大小表达。

### 7. 为什么不直接引用 TcpConnection 的读 Buffer？

那会让 RPC 层依赖 TCP 层内部 Buffer 的生命周期和读索引，并且阻塞客户端、协程客户端仍需另外拥有 Buffer。当前值成员虽然多一次复制，但所有权和复用关系更简单。

### 8. 类只有一个函数，为什么不删除？

判断抽象是否必要要看它是否封装真实状态。Connection 持有跨多次网络回调存在的半包 Buffer，并统一了服务端和两种客户端的循环拆帧逻辑，所以不是无意义的转发类。

# 面试表达

> binary::Connection 是单条 RPC 字节流的协议状态，不拥有 Socket。每条连接独占一个 Buffer，新字节先追加进去，再循环调用 FrameCodec 提取完整帧；数据不足时 FrameCodec 不消费 Buffer，所以半包自然留到下次，连续 Complete 则自然解决粘包。Connection 最终只向上层报告字节流是否有效，使 Server 和两种客户端共享同一套简单分帧逻辑。
