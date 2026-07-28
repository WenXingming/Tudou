# BinaryRpc - Server 设计：Reactor 编排、连接状态与错误关闭

`binary::Server` 是二进制 RPC 服务端的编排层。它建立在 `TcpServer` 之上，把连接级分帧、RPC 请求解析、Protobuf 业务路由和响应发送串成一条直接的主流程，但不重复实现这些模块的内部逻辑。

# Situation — 情境

一个二进制 RPC 请求从网络到业务响应需要经过多个阶段：

```text
TCP 字节流
  → 处理半包和粘包
  → 得到完整 Frame
  → 解析 service/method 调用头
  → 调用 Protobuf Service
  → 构造响应 Frame
  → 写回 TCP
```

如果 Server 自己保存字节缓冲、解析帧头并操作 Protobuf Descriptor，它会同时承担网络、协议和业务反射，逐渐成为上帝类。反过来，如果把每一两行代码都封装成 helper，主流程又会被隐藏在调用链中。

# Task — 任务

Server 需要满足以下约束：

- 基于现有 `TcpServer` 复用 Reactor、多线程连接管理和非阻塞 I/O；
- 每条 TCP 字节流拥有独立的 RPC 分帧状态；
- `on_message()` 直接展示网络请求到响应的完整处理顺序；
- 连接表只在必要范围内加锁，业务执行期间不持锁；
- 响应携带原请求的 `sequenceId`，支持客户端并发请求关联；
- 非法协议或业务异常统一关闭当前连接；
- 连接状态只在关闭回调中清理，不维护重复关闭状态。

# Action — 设计与实现

## 1. Server 只负责组合与编排

Server 的核心成员对应三个不同职责：

```cpp
TcpServer tcpServer_;  // TCP 监听、连接生命周期和网络收发
Router router_;        // Protobuf Service/Method 反射分发

std::mutex connectionsMutex_;
std::unordered_map<TcpConnection*, std::shared_ptr<Connection>> connections_;
```

职责边界为：

| 类 | 负责 | 不负责 |
| :--- | :--- | :--- |
| `TcpConnection` | Socket、Channel、TCP 收发与关闭 | RPC 帧和业务调用 |
| `binary::Connection` | 保存单连接半包并输出完整 Frame | Socket、路由和发送 |
| `Router` | `Request → Protobuf Service → response body` | Frame、sequenceId 和网络 |
| `Server` | 连接状态管理和完整调用编排 | 重复实现上述细节 |

`binary::Connection` 不持有 `TcpConnection`。如果只是为了隐藏 `conn->send()` 而保存网络引用，会引入新的所有权关系和一层无意义转发。网络 I/O 仍由接收 TCP 回调的 Server 统一编排，更直接。

## 2. 用 TcpServer 回调接入 Reactor

构造时，Server 把 TCP 层的三个生命周期事件绑定到自身：

```cpp
tcpServer_.set_connection_callback([this](const TcpConnectionPtr& conn) {
    on_connection(conn);
});
tcpServer_.set_message_callback([this](const TcpConnectionPtr& conn) {
    on_message(conn);
});
tcpServer_.set_close_callback([this](const TcpConnectionPtr& conn) {
    on_close(conn);
});
```

因此 Server 不创建 Socket、不调用 `epoll_wait()`，也不管理 Channel。底层 Reactor 在连接所属 EventLoop 中触发回调，Server 只处理 RPC 层语义：

```text
建立连接  → 创建 binary::Connection
收到数据  → 解帧、路由、回包
连接关闭  → 删除 binary::Connection
```

## 3. 每条 TCP 连接对应一个 RPC Connection

TCP 连接建立后，Server 创建独立的 RPC 分帧状态：

```cpp
void Server::on_connection(const TcpConnectionPtr& conn) {
    auto rpcConnection = std::make_shared<Connection>();
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_[conn.get()] = std::move(rpcConnection);
}
```

映射的含义是：

```text
TcpConnection*                  → shared_ptr<binary::Connection>
底层连接的稳定身份               → 该字节流独有的半包缓存
```

裸指针 key 只用于标识连接，不承担所有权；value 中的 `shared_ptr` 才负责 RPC 状态的生命周期。

不能让所有 TCP 连接共用一个 `Connection`，因为不同字节流的半包会互相污染。也不能在每次 `on_message()` 中临时创建 `Connection`，否则上一轮留下的半包会丢失。

## 4. 为什么 RPC 连接表仍然需要 mutex

`TcpServer` 可以把不同连接分配到不同 EventLoop，因而 Server 的连接建立、消息和关闭回调可能同时运行在多个 I/O 线程。这里是一张所有线程共享的 `unordered_map`，STL 容器本身不支持并发插入、查找和删除，所以必须加锁。

锁只保护映射操作：

```cpp
std::shared_ptr<Connection> rpcConnection;
{
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    const auto it = connections_.find(conn.get());
    if (it == connections_.end()) {
        return;
    }
    rpcConnection = it->second;
}
```

解锁后，局部 `shared_ptr` 保证 `Connection` 在本轮解析期间仍然存活。随后执行的分帧、Protobuf 反射和业务函数都不占用连接表锁。

这与 TCP 层“按 EventLoop 分片的无 mutex 连接表”并不矛盾：TCP 层为每个 EventLoop 使用独立内层容器；RPC Server 当前只有一张共享表，因此选择最小 mutex，而不是复制一套收益不明确的分片结构。

## 5. `on_message()` 平铺完整业务流程

主流程按真实数据变化顺序排列：

```text
TcpConnection::receive()
  → 查找本连接的 binary::Connection
  → Connection::decode() 提取全部完整 Frame
  → parse_request() 将 Request Frame 转为 Request
  → Router::dispatch() 执行业务 Service
  → send_response() 编码并发送响应 Frame
```

代码使用早返回处理失败，不引入额外状态机：

```cpp
std::vector<Frame> frames;
if (!rpcConnection->decode(data, frames)) {
    conn->force_close();
    return;
}

for (const auto& frame : frames) {
    Request request;
    if (!parse_request(frame, request)) {
        conn->force_close();
        return;
    }

    try {
        const std::string responseBody = router_.dispatch(request);
        send_response(conn, frame.header.sequenceId, responseBody);
    }
    catch (const std::exception&) {
        conn->force_close();
        return;
    }
}
```

`Connection::decode()` 内部已经解决半包和粘包，因此 Server 只需遍历本轮产生的完整 Frame。它不接触 Buffer 读索引，也不维护“当前解析到哪一步”的状态。

## 6. `parse_request()` 只完成 Frame 到 Request 的转换

一个请求 Frame 由以下语义组成：

```text
Frame.header.type       → 必须是 Request
Frame.head              → Protobuf CallHead：service_name + method_name
Frame.body              → 具体业务请求 Message 的序列化字节
```

`parse_request()` 验证帧类型、解析 `CallHead`，然后构造 Router 所需的逻辑请求：

```cpp
bool Server::parse_request(const Frame& frame, Request& request) {
    if (frame.header.type != FrameType::Request) {
        return false;
    }

    CallHead head;
    if (!head.ParseFromString(frame.head)) {
        return false;
    }

    request = Request(head.service_name(), head.method_name(), frame.body);
    return true;
}
```

它不反序列化具体业务 body，因为只有 Router 根据 service/method 找到 MethodDescriptor 后，才能确定 body 对应的具体 Protobuf Message 类型。

## 7. sequenceId 原样回传，实现请求与响应关联

Server 不生成新的响应序列号，而是复制请求帧的 `sequenceId`：

```cpp
const std::string responseBody = router_.dispatch(request);
send_response(conn, frame.header.sequenceId, responseBody);
```

```cpp
void Server::send_response(const TcpConnectionPtr& conn,
                           uint64_t sequenceId,
                           const std::string& responseBody) {
    const Frame response(FrameType::Response, sequenceId, "", responseBody);
    conn->send(FrameCodec::encode(response));
}
```

客户端允许在一条 TCP 连接上同时存在多个 pending call。响应到达后，客户端 `Multiplexer` 使用这个 `sequenceId` 找回对应等待者，因此响应不需要严格按照请求发起顺序完成。

Server 只负责原样回传 ID；pending call 的存储和唤醒属于客户端，不进入服务端连接状态。

## 8. 关闭动作与状态清理只有一条路径

当前协议没有独立的错误响应 Frame。遇到以下情况时，Server 采用 fail-closed 策略：

- 帧头或长度非法；
- 收到的 Frame 不是请求类型；
- `CallHead` 无法解析；
- service/method 不存在；
- 请求 body 非法；
- 业务方法抛出异常或没有同步完成。

错误分支只负责发起关闭：

```cpp
conn->force_close();
return;
```

随后 TCP 层触发统一关闭回调：

```text
Server 调用 force_close()
  → TcpConnection 完成关闭
  → TcpServer::on_close()
  → binary::Server::on_close()
  → connections_.erase(conn.get())
```

被动断开也进入同一个 `on_close()`。因此 Server 不需要同时维护 `remove_connection()` 和 `close_connection()` 两套 helper，也不会在主动关闭前后重复清理 RPC 状态。

## 9. 线程数量参数表达总 EventLoop 数

`TcpServer` 的构造参数表示额外 I/O Loop 数，其内部始终还有一个 main EventLoop。`binary::Server` 的 `numThreads` 则对外表示总 EventLoop 数，所以构造时进行一次换算：

```cpp
tcpServer_(ip, port, numThreads > 0 ? numThreads - 1 : 0)
```

例如：

```text
Server numThreads = 0 或 1 → TcpServer 额外线程数 0 → 共 1 个 EventLoop
Server numThreads = 4      → TcpServer 额外线程数 3 → 共 4 个 EventLoop
```

这种换算只出现在组合边界，不让上层调用者理解 TcpServer 的内部计数方式。

# Result — 结果

- Server 的核心流程保持为 `receive → decode → parse → dispatch → send`；
- TCP、分帧和 Protobuf 反射分别由专门类实现；
- 每条字节流拥有独立半包状态，粘包可以在一次回调中连续处理；
- 共享连接表只在增删查时加锁，业务调用不持锁；
- `sequenceId` 原样回传，支持客户端单连接多路复用；
- 主动关闭和被动断开统一通过 `on_close()` 清理状态；
- 没有为错误响应、异步 Service 或连接表分片提前增加状态机。

`BinaryRpcServerTest.ExecutesCompleteRequest` 使用真实本地 TCP 连接验证完整闭环：发送带 `sequenceId = 8888` 的 Echo 请求帧，服务端执行注册的 Protobuf Service，客户端解出 Response Frame，并验证响应类型、相同序列号和业务响应内容。

# 权衡与边界

- 当前 RPC 连接表使用最小 mutex，不宣称无锁；若性能数据证明它成为热点，再考虑按 EventLoop 分片。
- 当前没有协议级错误响应，服务端错误以关闭连接表示，客户端需要让该连接上的全部 pending call 失败。
- Router 当前只支持同步 Protobuf Service，业务方法必须在返回前调用 `done->Run()`。
- Service 可能由多个 I/O 线程并发调用，业务实现需要自行保证线程安全，并避免长时间阻塞 EventLoop。
- `register_service()` 应在 `start()` 前完成；当前没有为运行期并发注册增加锁和动态配置语义。

# 面试核心问答

### 1. Server 在二进制 RPC 中负责什么？

它是编排层：监听和网络事件复用 TcpServer，半包与粘包交给 Connection，Protobuf 反射调用交给 Router；自身负责连接状态表、Frame 到 Request 的转换、sequenceId 回传和错误关闭。

### 2. 为什么 Server 不直接解析 TCP Buffer？

半包是跨多次回调存在的连接级状态。Connection 独占一个 Buffer，并复用 FrameCodec 循环提取完整帧。Server 只消费完整 Frame，可以保持主流程清晰。

### 3. 为什么每条 TcpConnection 都需要一个 binary::Connection？

因为每条 TCP 字节流都可能留下不同的半包。独立 Connection 保证不同连接的字节不会混合，并让上一轮剩余数据能与下一轮新数据继续拼接。

### 4. RPC 连接表为什么需要 mutex？TcpServer 的连接表不是无 mutex 吗？

RPC Server 当前使用一张被多个 EventLoop 共享的 `unordered_map`，必须加锁。TcpServer 使用的是按 EventLoop 分片的多个内层容器，每个容器只由 owner loop 访问，两者结构不同，不能套用同一个并发结论。

### 5. 查表后为什么复制一个 shared_ptr 再释放锁？

锁只保护 map 的结构安全；局部 shared_ptr 保证解锁后 Connection 仍然存活。这样耗时的解帧和业务调用不会阻塞其他连接的建立、查找或关闭。

### 6. 为什么 binary::Connection 不持有 TcpConnection 并负责发送？

Connection 当前是纯协议状态，只负责半包与粘包。若只为包装一行 `TcpConnection::send()` 而保存网络引用，会增加所有权和生命周期复杂度。发送作为 Server 编排流程的最后一步更直接。

### 7. sequenceId 在服务端有什么作用？

Server 把请求 ID 原样写入响应帧。客户端收到响应后用它定位对应的 pending call，从而允许一条 TCP 连接上同时存在多个并发 RPC 请求。

### 8. 为什么错误时直接关闭连接，而不是继续解析后续 Frame？

帧或调用头非法后，当前字节流的协议可信度已经丢失；路由和业务异常又没有对应的错误帧格式。关闭连接可以让客户端统一失败全部 pending call，避免等待永远不会到达的响应。

### 9. `force_close()` 后为什么不立即手动删除 RPC Connection？

`force_close()` 会经过 TcpConnection 和 TcpServer 的关闭链，最终触发 Server::on_close()。主动关闭与对端断开都在这个回调中清理，避免维护两套路径和重复 erase。

### 10. 业务逻辑写在哪里？

业务代码继承 `protoc` 生成的 Service 基类并重写具体方法，然后在启动前调用 `register_service()`。Server 不包含业务逻辑；它经 Router 的 `CallMethod()` 分发入口调用用户重写的方法。

# 面试表达

> 二进制 RPC Server 是 Reactor 之上的编排层。TcpServer 管非阻塞网络和连接生命周期，每条连接有独立 Connection 保存半包并拆出完整 Frame，Router 用 Protobuf 反射执行用户注册的 Service。Server 的主流程平铺为 receive、decode、parse、dispatch、send；共享 RPC 连接表只在查表和增删时加锁，解锁后靠 shared_ptr 保活。响应原样携带请求 sequenceId 支持单连接多路复用，协议或业务失败统一 force_close，并只在 on_close 中清理状态。
