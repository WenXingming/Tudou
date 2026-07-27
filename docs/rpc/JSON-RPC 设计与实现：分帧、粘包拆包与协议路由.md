# JSON-RPC 设计与实现：分帧、粘包拆包与协议路由

对应源码：[`src/tudou/rpc/`](file:///home/wxm/Tudou/src/tudou/rpc/)

## 1. 模块概述与设计定位

`jsonrpc` 模块是 Tudou 网络库中实现的轻量级 JSON-RPC 2.0 协议框架。

与 Protobuf 二进制 RPC 相比，JSON-RPC 的核心优势在于：

1. **免 IDL 编译**：无需通过 `protoc` 编译 `.proto` 文件生成桩代码（Stub），开发者注册回调函数即可提供 RPC 服务。
2. **跨语言极其友好**：任何支持 TCP 与 JSON 的语言（Python、Node.js、Go、Java）均可零成本发起调用。
3. **可读性与调试便利**：网络传参为明文 JSON 文本，配合抓包工具（如 `tcpdump` / `Wireshark`）可直接查看与调试。

### 核心组件划分

- **[`JsonRpcClient`](file:///home/wxm/Tudou/src/tudou/rpc/JsonRpcClient.h)**：基于 TCP 的同步阻塞 Ping-Pong 客户端，采用 RAII 资源包装与 SRP 单一职责分离设计。
- **[`JsonRpcRouter`](file:///home/wxm/Tudou/src/tudou/rpc/JsonRpcRouter.h)**：纯内存协议路由分发器，负责 JSON-RPC 2.0 规范校验、单/批量请求分发、回调调度与标准错误码封装。
- **[`JsonRpcServer`](file:///home/wxm/Tudou/src/tudou/rpc/JsonRpcServer.h)**：服务端组件，组合底层的 [`TcpServer`](file:///home/wxm/Tudou/src/tudou/tcp/TcpServer.h)，管理多连接接收缓冲区并调度 Router。

---

## 2. JSON-RPC 2.0 协议规范与契约

模块严格遵循 [JSON-RPC 2.0 官方规范](https://www.jsonrpc.org/specification)。

### 消息格式示例

- **请求包（Request）**：
  ```json
  {"jsonrpc": "2.0", "method": "add", "params": [10, 20], "id": 101}
  ```
- **成功响应包（Success Response）**：
  ```json
  {"jsonrpc": "2.0", "result": 30, "id": 101}
  ```
- **错误响应包（Error Response）**：
  ```json
  {"jsonrpc": "2.0", "error": {"code": -32601, "message": "Method not found"}, "id": 101}
  ```
- **通知请求（Notification）**：不包含 `id` 字段的请求。服务端接收并执行业务 Handler 后，规范规定**无需产生任何响应包**。
- **批量请求（Batch Request）**：包含多个请求对象的 JSON 数组 `[...]`，服务端依次分发并聚合非 Notification 的响应为一个 JSON 数组返回。

### 强类型错误码定义 (`JsonRpcErrorCode`)

在 [`JsonRpcRouter.h`](file:///home/wxm/Tudou/src/tudou/rpc/JsonRpcRouter.h) 中将规范错误码定义为强类型枚举：

```cpp
enum class JsonRpcErrorCode {
    ParseError     = -32700, // JSON 语法解析错误
    InvalidRequest = -32600, // 结构非对象/版本非 2.0 等非法格式
    MethodNotFound = -32601, // 调用的方法未在 Router 注册
    InvalidParams  = -32602, // 传入参数类型与业务 Handler 需求不符
    InternalError  = -32603  // 业务 Handler 执行期间抛出了 C++ 未捕获异常
};
```

---

## 3. 分帧、拆包与粘包处理机制

由于 TCP 是面向无边界字节流的传输层协议，应用层必须定义清晰的消息帧边界。

### 3.1 行分隔符分帧 (Line-Delimited JSON)

Tudou JSON-RPC 选用 **换行符 `\n` (ASCII 0x0A)** 作为单条消息的结束分隔符。

> **安全性验证**：`nlohmann::json::dump()` 序列化出来的 JSON 文本内部若包含换行文本（如 `"hello\nworld"`），会被序列化引擎自动**转义**为 `\` (0x5C) 和 `n` (0x6E) 两个字符，而**绝不会产生原始的 0x0A 字节**。因此，网络流中唯一的 0x0A 字节必然是帧尾追加的 `\n`。

### 3.2 客户端解包机制 (`JsonRpcClient::read_line`)

`JsonRpcClient` 维持一个内部接收缓冲区 `recvBuf_`：

```cpp
std::string JsonRpcClient::read_line() {
    char temp[512];
    size_t delimiterPos = recvBuf_.find('\n');
    while (delimiterPos == std::string::npos) {
        ssize_t nr = ::read(clientFd_.fd(), temp, sizeof(temp));
        if (nr <= 0) {
            if (nr < 0 && errno == EINTR) continue;
            throw std::runtime_error("JsonRpcClient: Connection closed by remote server...");
        }
        recvBuf_.append(temp, nr);
        delimiterPos = recvBuf_.find('\n');
    }

    std::string line = recvBuf_.substr(0, delimiterPos);
    recvBuf_.erase(0, delimiterPos + 1);
    return line;
}
```

- **拆包（半包）处理**：当 TCP 接收到的数据暂无 `\n` 时，`recvBuf_.find('\n')` 返回 `npos`。`while` 循环保持阻塞 `::read` 并追加到 `recvBuf_`，直到拼出完整的带 `\n` 单行消息退出。
- **粘包处理说明**：`JsonRpcClient` 采用**同步阻塞（Ping-Pong 一发一收）**模型。客户端发出请求 1 后会阻塞等待响应 1，在未收到响应 1 前不可能发送请求 2。因此在正常 Ping-Pong 客户端下**不会发生多个响应粘包**；`recvBuf_.erase()` 机制保障了协议防御安全性。

### 3.3 服务端拆包与粘包处理 (`JsonRpcServer::extract_lines`)

服务端面对并发或支持 Pipeline（流水线连续发送）的客户端时，一次 TCP `conn->receive()` 可能收多条以 `\n` 结尾的 JSON 请求。

`JsonRpcServer` 维护了线程安全的连接接收缓存 `connectionBuffers_`：

```cpp
std::vector<std::string> JsonRpcServer::extract_lines(TcpConnection* connKey, const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string& connBuf = connectionBuffers_[connKey];
    connBuf.append(data);

    std::vector<std::string> lines;
    while (true) {
        size_t pos = connBuf.find('\n');
        if (pos == std::string::npos) break;

        std::string requestStr = connBuf.substr(0, pos);
        connBuf.erase(0, pos + 1);

        if (!requestStr.empty()) {
            lines.push_back(std::move(requestStr));
        }
    }
    return lines;
}
```

- 互斥锁 `mutex_` 保证了 IO 多路复用与多线程 Reactor 回调下，连接缓冲区的修改原子性。
- `while` 循环提取出包内所有的完整请求行并存入 `lines` 数组，主回调 `on_message` 依次循环调用 `router_.dispatch()` 完成解包与分发。

---

## 4. 核心组件实现与架构优化

### 4.1 `JsonRpcClient`：RAII 资源与单一职责 (SRP) 解耦

1. **RAII 资源管理**：使用 [`ScopedFd`](file:///home/wxm/Tudou/src/base/ScopedFd.h) 接管套接字文件描述符 `clientFd_`。构造函数异常（如 `connect` 失败）引发栈展开时，`ScopedFd` 自动释放 socket，彻底杜绝句柄泄漏。
2. **SRP 流水线解耦**：
   ```cpp
   nlohmann::json JsonRpcClient::call(const std::string& method, const nlohmann::json& params) {
       uint64_t seq = nextSequenceId_++;
       std::string requestStr = encode_request(method, params, seq); // 1. 纯内存协议编码
       send_all(requestStr);                                         // 2. 纯 Socket 发送
       std::string responseLine = read_line();                       // 3. 纯 Socket 接收
       return decode_response(responseLine, seq);                    // 4. 纯内存协议解码与校验
   }
   ```

### 4.2 `JsonRpcRouter`：纯内存分发与平铺架构

`JsonRpcRouter` 是应用层路由状态机，将网络传输与协议逻辑完全隔离开：

- **平铺分发 (`dispatch`)**：处理为空防空、尝试 JSON Parse 捕获解析异常（映射为 `-32700`）、识别 `is_array` 分发给 `dispatch_batch`，或调用 `dispatch_single`。
- **单请求 7 步校验法则 (`dispatch_single`)**：
  1. Object 根类型校验 ➔ `-32600`
  2. Notification / `id` 校验 ➔ `-32600`
  3. `jsonrpc: "2.0"` 及 `method` 校验 ➔ `-32600`
  4. 路由表中匹配 `methods_.find(methodName)` ➔ `-32601`
  5. `params` 参数类型校验 ➔ `-32602`
  6. 调度回调 Handler 捕获业务异常 ➔ `-32602` / `-32603`
  7. 成功返回（Notification 返回 `nullptr`；普通请求调 `make_success_response`）

---

## 5. 跨语言通信实践

在 [`examples/JsonRpcServer/`](file:///home/wxm/Tudou/examples/JsonRpcServer/) 中提供了完整的跨语言通信用例：

- **服务端**（C++ [`main.cpp`](file:///home/wxm/Tudou/examples/JsonRpcServer/main.cpp)）：
  ```cpp
  JsonRpcServer server("127.0.0.1", 8090, 2);
  server.register_method("add", [](const nlohmann::json& params) {
      return params[0].get<int>() + params[1].get<int>();
  });
  server.start();
  ```
- **客户端**（Python [`client.py`](file:///home/wxm/Tudou/examples/JsonRpcServer/client.py)）：
  ```python
  import socket, json

  s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
  s.connect(("127.0.0.1", 8090))
  payload = json.dumps({"jsonrpc": "2.0", "method": "add", "params": [45, 55], "id": 100}) + "\n"
  s.sendall(payload.encode("utf-8"))

  resp = s.recv(1024).decode("utf-8").strip()
  print("Response:", json.loads(resp)) # {"id": 100, "jsonrpc": "2.0", "result": 100}
  ```
