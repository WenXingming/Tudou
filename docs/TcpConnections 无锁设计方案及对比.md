# TcpConnections 无锁设计方案及对比

本文记录两套连接表无锁设计：

1. muduo 的集中式连接账本：`TcpServer::connections_` 只归主 `EventLoop` 线程访问。
2. Tudou 当前的分片式连接账本：`connectionRecordsByLoop_` 外层作为只读索引，内层 `ConnectionRecords` 归各自 owner `EventLoop` 线程访问。

文档采用 STAR 法则组织：Situation（背景）、Task（目标）、Action（做法）、Result（结果），最后给出对比和安全边界。

---

## 一、先给判断标准

讨论 `connections_` 或 `connectionRecordsByLoop_` 是否需要加锁，不能只看“它是一个容器”，也不能只看“程序有多个线程”。真正的判断标准是：

> 是否存在多个线程在同一时刻直接访问同一个可变对象，并且至少有一个线程在写。

在 Reactor 网络库里，常见的无锁方式不是让容器变成“并发容器”，而是把状态归属到某个 `EventLoop` 线程：

- 归属线程内直接访问。
- 其他线程不能直接改状态，只能通过 `runInLoop` / `queueInLoop` 投递任务。
- 真正需要同步的地方收敛到跨线程任务队列、`eventfd` 唤醒、少量 atomic 状态。

所以，“不加锁”成立的前提不是容器本身神奇，而是线程归属规则足够严格。

---

## 二、muduo 的设计：主 loop 集中式连接账本

### Situation - 背景

muduo 是典型的 one loop per thread Reactor 设计。`TcpServer` 有一个主 `EventLoop`，负责监听 socket、accept 新连接，并持有全部存活连接的连接表：

```cpp
typedef std::map<string, TcpConnectionPtr> ConnectionMap;

// always in loop thread
ConnectionMap connections_;
```

这里的关键注释是 `always in loop thread`。它明确说明 `connections_` 不是一个可以被任意线程访问的共享容器，而是主 `loop_` 线程拥有的账本。

### Task - 目标

muduo 要解决的问题是：

1. 新连接由主 loop accept，但连接 I/O 可以分发到多个 ioLoop。
2. 连接关闭通常发生在连接所属 ioLoop 线程。
3. `TcpServer` 仍然需要保存所有连接，避免连接对象提前析构，并在关闭时统一移除。
4. 不能让多个 ioLoop 直接并发修改 `connections_`，否则必须给连接表加锁。

因此 muduo 的目标不是“让 `connections_` 支持并发访问”，而是“让所有 `connections_` 的读写都回到主 loop 线程”。

### Action - 做法

#### 1. 新连接插入只在主 loop 线程执行

`Acceptor` 绑定在主 loop 上。监听 fd 可读时，主 loop 执行 accept，然后直接触发 `TcpServer::newConnection`。

`newConnection` 的核心流程是：

```text
main loop
  -> Acceptor::handleRead
  -> TcpServer::newConnection
  -> 选择 ioLoop
  -> 创建 TcpConnection
  -> connections_[connName] = conn
  -> ioLoop->runInLoop(TcpConnection::connectEstablished)
```

注意，虽然新连接最终会交给某个 ioLoop 处理 I/O，但“登记到 `connections_`”这一步仍然发生在主 loop 线程。

#### 2. 关闭连接时，ioLoop 不直接删除主连接表

连接关闭事件发生在连接所属 ioLoop，例如读到 EOF、发生错误或主动关闭。此时 `TcpConnection` 触发 close callback。

muduo 不让这个 ioLoop 线程直接 erase `connections_`，而是走下面的路径：

```text
owner ioLoop
  -> TcpConnection::handleClose
  -> closeCallback_
  -> TcpServer::removeConnection(conn)
  -> server loop_->runInLoop(TcpServer::removeConnectionInLoop)

main loop
  -> TcpServer::removeConnectionInLoop(conn)
  -> connections_.erase(conn->name())
  -> ioLoop->queueInLoop(TcpConnection::connectDestroyed)
```

`removeConnection` 是跨线程入口，但它不直接改 map。真正的 `connections_.erase(...)` 在 `removeConnectionInLoop` 里执行，而该函数由主 `loop_` 调度。

#### 3. 同步点在 EventLoop 的任务队列，不在 connections_ 上

muduo 的 `EventLoop::runInLoop` 语义是：

- 如果调用者就在目标 loop 线程，直接执行。
- 如果调用者不在目标 loop 线程，转为 `queueInLoop`，把任务放入目标 loop 的 pending functors，并通过 `eventfd` 唤醒。

所以跨线程同步实际发生在任务投递边界。`connections_` 本身仍然是普通 `std::map`，不需要内部加锁。

#### 4. `started_` 和 `connections_` 的同步策略不同

muduo 的 `TcpServer::start()` 被设计为可跨线程调用，所以 `started_` 使用 atomic。`connections_` 则没有这个属性，它只属于主 loop 线程。

这说明一个重要原则：

> 同一个类里的不同成员，可以有不同的线程安全策略。不是类里出现多线程，就所有成员都必须加锁。

### Result - 结果

muduo 得到的是一套非常容易推理的集中式账本模型：

- `connections_` 无锁，因为只由主 loop 线程读写。
- ioLoop 线程不直接改 server 连接表，只投递删除任务。
- 连接对象生命周期由 `TcpServer` 的 `shared_ptr` 连接表兜住。
- 模型简单，容易验证正确性。

代价也很清楚：

- 所有连接的插入和删除最终都要经过主 loop。
- 连接关闭发生在 ioLoop 时，需要额外投递一次任务回主 loop。
- 如果连接创建/关闭频率极高，主 loop 的 pending functor 队列可能成为调度热点。

这不是 bug，而是 muduo 在简单性、可推理性和性能之间做出的取舍。

---

## 三、Tudou 当前设计：外层只读索引 + 内层分片账本

### Situation - 背景

Tudou 当前实现不是 muduo 的“主 loop 集中式账本”，而是更接近分片自治：

```cpp
struct ConnectionRecord {
    TcpConnectionPtr connection;
    std::shared_ptr<ConnectionHeartbeat> heartbeat;
};

using ConnectionRecords = std::unordered_map<TcpConnection*, ConnectionRecord>;

std::unordered_map<EventLoop*, ConnectionRecords> connectionRecordsByLoop_;
std::atomic<size_t> activeConnectionCount_;
std::atomic<ServerState> state_;
```

外层 key 是 `EventLoop*`，内层 `ConnectionRecords` 是这个 loop 拥有的连接记录表。

当前实现里，`TcpServer::start()` 会先启动线程池并拿到所有 loop，然后一次性初始化外层表：

```text
TcpServer::start
  -> loopThreadPool_->start()
  -> get_all_loops()
  -> connectionRecordsByLoop_.reserve(loops.size())
  -> for each loop: emplace(loop, ConnectionRecords())
  -> state_ = Running
  -> mainLoop.loop()
```

也就是说，服务器真正开始 accept 连接之前，外层 `EventLoop* -> ConnectionRecords` 索引已经建好。

### Task - 目标

Tudou 这版连接表设计要解决的问题更进一步：

1. 连接创建、消息处理、关闭和心跳刷新尽量都留在连接所属 ioLoop。
2. 避免连接关闭时再投递回 main loop 删除全局连接表。
3. 心跳对象和连接对象绑定到同一个 owner loop，避免刷新心跳时触碰全局锁。
4. shutdown 时仍然能从 `TcpServer` 统一收口所有连接。
5. 保留普通 STL 容器，避免引入复杂并发容器。

核心目标是：

> 让连接的“生老病死”在 owner loop 内形成闭环；`TcpServer` 只保存分片索引和 shutdown 所需的全局计数。

### Action - 做法

#### 1. 外层 map 启动期构建，运行期只读

`connectionRecordsByLoop_` 的外层表在 `start()` 中一次性构建：

```text
connectionRecordsByLoop_.reserve(loops.size())
for (EventLoop* loop : loops) {
    connectionRecordsByLoop_.emplace(loop, ConnectionRecords())
}
```

运行期代码只用 `find(loop)` 定位分片：

- `on_connect` 里确认 `ioLoop` 已存在。
- `create_connection` 里 `find(&ioLoop)`。
- `remove_connection` 里 `find(conn->get_loop())`。
- `refresh_connection_heartbeat` 里 `find(conn->get_loop())`。

它不再插入、不删除、不 rehash，也不使用 `operator[]` 这种可能隐式插入的接口。

因此外层 map 虽然会被多个线程读，但它在运行期是只读索引。多个线程并发只读同一个标准库容器是可以成立的；真正危险的是“一个线程读，另一个线程改结构”。

#### 2. 新连接不是在 main loop 建立记录，而是投递到目标 ioLoop

Tudou 的 accept 仍然发生在 main loop：

```text
main loop
  -> Acceptor accept
  -> TcpServer::on_connect
  -> loopThreadPool_->get_next_loop()
```

但它不会直接在 main loop 里创建 `TcpConnection` 和修改内层表，而是把 socket 所有权投递给目标 ioLoop：

```text
main loop
  -> ioLoop->run_in_loop(lambda)

owner ioLoop
  -> TcpServer::create_connection(*ioLoop, socket, peerAddr)
  -> localRecords[conn.get()] = ConnectionRecord{ conn, heartbeat }
  -> activeConnectionCount_.fetch_add(1)
  -> heartbeat->start()
```

这一步是当前设计和 muduo 的关键差异：

- muduo：主 loop 创建连接并写主连接表，再让 ioLoop 激活连接。
- Tudou：主 loop 只分配目标 loop，真正创建连接和写分片表发生在目标 ioLoop。

#### 3. 内层 ConnectionRecords 只由 owner loop 读写

内层表的可变操作都带有 owner loop 线程归属：

```text
create_connection
  -> assert(ioLoop.is_in_loop_thread())
  -> localRecords[conn.get()] = ...

remove_connection
  -> assert(conn->get_loop()->is_in_loop_thread())
  -> localRecords.erase(it)

refresh_connection_heartbeat
  -> assert(conn->get_loop()->is_in_loop_thread())
  -> localRecords.find(conn.get())
  -> heartbeat->refresh()
```

因此每个 `ConnectionRecords` 的真实拥有者是它对应的 `EventLoop` 线程，不是所有线程共同拥有。

换句话说，`connectionRecordsByLoop_` 的二层结构不是“一个大共享 hash 表套一个小共享 hash 表”，而是：

```text
外层：运行期只读目录
内层：每个 loop 独占的本地账本
```

#### 4. 关闭连接不再回 main loop 删除连接表

Tudou 的连接关闭路径是：

```text
owner ioLoop
  -> TcpConnection::close_connection
  -> TcpConnection::handle_close_callback
  -> TcpServer::on_close(conn)
  -> TcpServer::remove_connection(conn)
  -> localRecords.erase(conn.get())
  -> activeConnectionCount_.fetch_sub(1)
  -> heartbeat->stop()
  -> closeCallback_(conn)
```

由于 `TcpConnection` 的 fd 事件由 owner ioLoop 分发，close callback 也在 owner ioLoop 执行，所以 `remove_connection` 可以直接删除本地分片，不需要再投递回 main loop。

这减少了关闭路径上的跨线程任务投递。

#### 5. 心跳对象也跟随连接分片

每条连接可以持有一个独立 `ConnectionHeartbeat`：

```text
ConnectionRecord
  -> TcpConnectionPtr connection
  -> shared_ptr<ConnectionHeartbeat> heartbeat
```

心跳刷新发生在消息回调前：

```text
owner ioLoop
  -> TcpServer::on_message(conn)
  -> refresh_connection_heartbeat(conn)
  -> messageCallback_(conn)
```

`ConnectionHeartbeat` 内部的定时器也绑定在连接所属 `EventLoop` 上。这样心跳的 `start`、`refresh`、`stop` 都和连接状态处在同一个线程边界内。

#### 6. shutdown 通过投递到各分片收口

`TcpServer::stop()` 把状态从 `Running` CAS 到 `Draining`，然后请求 main loop 退出。

`start()` 中的 `mainLoop.loop()` 返回后，执行 `shutdown_connections()`：

```text
main thread
  -> state_ = Draining
  -> 遍历外层 connectionRecordsByLoop_
  -> 对每个 loop 投递 shutdown lambda

owner loop
  -> 遍历自己的 localRecords
  -> 先把 TcpConnectionPtr 拷贝到 activeConnections
  -> 再逐个 conn->force_close()
  -> 关闭回调删除 localRecords

main thread
  -> 等待 activeConnectionCount_ == 0
  -> reset acceptor
  -> reset thread pool
  -> clear connectionRecordsByLoop_
```

这里先拷贝 `TcpConnectionPtr` 到局部 vector，再调用 `force_close()`，是为了避免在遍历 `localRecords` 的过程中触发 `erase` 导致迭代器失效。

`activeConnectionCount_` 是 shutdown 等待条件，不参与请求热路径。它的作用是告诉 main thread：所有分片里的连接记录都已经通过 `remove_connection` 被清掉。

### Result - 结果

Tudou 当前设计得到的是分片式无锁连接管理：

- 新连接记录在目标 ioLoop 中创建。
- 消息、心跳刷新、连接关闭都在 owner ioLoop 中完成。
- 连接关闭不需要回 main loop 删除全局连接表。
- 外层表运行期只读，内层表线程独占，因此连接表本身不需要 mutex。
- `activeConnectionCount_` 用 atomic 解决 shutdown 等待，不进入发送路径。

这套设计的性能取向比 muduo 更激进一些：它把连接账本更新下沉到了各个 ioLoop，减少 main loop 在连接关闭和心跳路径上的中转压力。

但它的正确性依赖更明确的使用契约：

- `connectionRecordsByLoop_` 外层表必须只在启动期构建、停止后清理，运行期不能增删 key。
- 内层 `ConnectionRecords` 只能在对应 owner loop 线程访问。
- `set_*_callback()`、`set_connection_heartbeat()` 这类配置接口应在 `start()` 前完成，运行期并发修改会和 ioLoop 读取产生数据竞争。
- `TcpServer` 对象必须活到 `start()` 返回之后，因为连接回调中捕获的是裸 `this`。

---

## 四、两套方案的核心对比

| 维度 | muduo 集中式账本 | Tudou 当前分片账本 |
| --- | --- | --- |
| 连接表形态 | 一个 `ConnectionMap connections_` | `EventLoop* -> ConnectionRecords` 二层表 |
| 可变账本归属 | 主 `loop_` 线程 | 每个 owner `EventLoop` 线程 |
| 外层索引 | 无 | 启动期构建，运行期只读 |
| 新连接登记 | main loop 插入 `connections_` | 目标 ioLoop 插入自己的 `localRecords` |
| 连接关闭删除 | ioLoop 触发关闭，投递回 main loop 删除 | owner ioLoop 直接删除自己的 `localRecords` |
| 心跳刷新 | muduo 原版无该连接心跳账本 | owner ioLoop 查本地 record 并刷新 heartbeat |
| shutdown 收口 | 析构/关闭时遍历集中连接表，再投递到连接 ioLoop | 遍历外层只读索引，把关闭任务投递给各分片 |
| 是否需要连接表锁 | 不需要，因为集中到主 loop | 不需要，因为外层只读、内层线程独占 |
| 跨线程同步点 | `EventLoop::runInLoop/queueInLoop` | `EventLoop::run_in_loop/queue_in_loop` + atomic 计数 |
| 优点 | 简单，容易证明，连接表只有一个 owner | 减少 main loop 中转，关闭/心跳路径局部化 |
| 代价 | 关闭路径需要回 main loop，主 loop 可能成为账本热点 | 使用契约更严格，生命周期和配置并发边界更容易被误用 |

---

## 五、为什么 Tudou 的一级哈希也不用加锁

这个问题最容易误判。`connectionRecordsByLoop_` 的一级哈希确实会被多个线程访问，但它不因此自动需要锁。

关键是运行期访问模式：

```cpp
std::unordered_map<EventLoop*, ConnectionRecords> connectionRecordsByLoop_;
```

外层 map 的生命周期分为三个阶段：

### 1. 构建阶段

`start()` 中：

```text
reserve
emplace(loop, ConnectionRecords())
```

这发生在服务器开始 accept 连接之前。

### 2. 运行阶段

运行期只做：

```text
connectionRecordsByLoop_.find(loop)
```

不插入、不删除、不 rehash、不使用 `operator[]`。

只要这个约束成立，外层表就是只读共享索引。多个线程同时 `find` 外层表，不会改变容器内部结构。

### 3. 清理阶段

`shutdown_connections()` 等待 `activeConnectionCount_` 归零后，`start()` 继续 reset 线程池并清空外层表：

```text
shutdown_connections()
loopThreadPool_.reset()
connectionRecordsByLoop_.clear()
```

清空发生在连接收口和线程池退出之后，不和运行期连接操作并发修改同一个外层 map。

所以一级哈希不加锁的真实原因是：

> 它不是运行期共享可变容器，而是启动后只读的 loop 分片目录。

如果以后改成运行期动态增删 ioLoop、热扩容线程池、或者用 `operator[]` 懒创建分片，那么这个结论立刻失效。

---

## 六、为什么 Tudou 的内层哈希不用加锁

内层 `ConnectionRecords` 是真正会增删改的连接账本。它能不加锁，靠的是 owner loop 独占。

以一个连接 `conn` 为例：

```text
conn->get_loop() == ioLoop0
```

那么它对应的 record 只能由 `ioLoop0` 线程访问：

```text
ioLoop0:
  create_connection     -> 插入 record
  on_message            -> 查 record，刷新 heartbeat
  on_close              -> 删除 record
  shutdown lambda       -> 遍历 record 并 force_close
```

其他线程如果想影响这条连接，不能直接操作 `localRecords`，只能：

- 对 `ioLoop0` 调用 `run_in_loop` / `queue_in_loop`。
- 调用 `TcpConnection::send()` / `force_close()` 这种会自动投递回 owner loop 的入口。
- 通过 `state_` / `activeConnectionCount_` 这种 atomic 变量表达跨线程状态。

因此内层表不是多线程共享账本，而是“线程私有账本挂在一个全局目录下面”。

---

## 七、不要把“连接表无锁”扩大成“整个 TcpServer 完全线程安全”

当前结论应该精确表述为：

> `connectionRecordsByLoop_` 在当前线程归属模型下不需要加锁。

不能扩大成：

> `TcpServer` 的所有成员函数都可以任意线程并发调用。

原因如下。

### 1. 配置回调不是运行期线程安全接口

下面这些成员没有加锁，也不是 atomic：

```cpp
ConnectionCallback connectionCallback_;
MessageCallback messageCallback_;
CloseCallback closeCallback_;
ErrorCallback errorCallback_;
WriteCompleteCallback writeCompleteCallback_;
HighWaterMarkCallback highWaterMarkCallback_;
size_t highWaterMark_;
ConnectionHeartbeatOptions connectionHeartbeatOptions_;
```

`create_connection`、`on_message`、`on_close`、`refresh_connection_heartbeat` 会在 ioLoop 线程读取它们。如果其他线程在 server 运行期间调用 `set_*_callback()` 或 `set_connection_heartbeat()` 修改它们，就会产生数据竞争。

因此它们的契约应该是：

```text
只在 start() 前配置。
server running 后不要并发修改。
```

### 2. 回调捕获裸 this，要求 TcpServer 生命周期覆盖所有连接任务

`TcpServer` 给 `TcpConnection` 设置回调时捕获了 `[this]`：

```cpp
conn->set_message_callback([this](const TcpConnectionPtr& activeConn) {
    on_message(activeConn);
});

conn->set_close_callback([this](const TcpConnectionPtr& activeConn) {
    on_close(activeConn);
});
```

这要求 `TcpServer` 对象必须活到所有连接回调和 shutdown 任务结束之后。

如果外部在 `start()` 仍运行、ioLoop 仍有挂起任务时销毁 `TcpServer`，连接表有没有锁都救不了，因为问题已经变成悬空 `this`。

### 3. 业务层长期保存 TcpConnectionPtr 仍需谨慎

当前 API 把 `TcpConnectionPtr` 暴露给上层。这样发送路径很高效：

```text
on_message(owner loop)
  -> conn->receive()
  -> conn->send(response)
```

但如果业务层把 `TcpConnectionPtr` 长期保存，并在 server shutdown 后继续调用 `conn->send()` 或 `conn->force_close()`，就可能碰到已经退出或销毁的 `EventLoop*`。

这不是连接表锁能解决的问题，而是 API 生命周期契约问题。

---

## 八、设计取舍总结

muduo 和 Tudou 当前方案都不是靠给连接表加锁来保证安全，而是靠“线程归属”保证安全。

muduo 的归属模型是：

```text
connections_ 归 server main loop
ioLoop 关闭连接时投递回 main loop 删除
```

Tudou 的归属模型是：

```text
connectionRecordsByLoop_ 外层归启动/停止阶段管理，运行期只读
每个 ConnectionRecords 归对应 owner loop
连接创建、心跳刷新、关闭删除都在 owner loop
```

两者都合理，只是取舍不同：

- muduo 更集中、更简单，适合作为基础 Reactor 模型。
- Tudou 更分片、更贴近 thread-per-core 的性能目标，但要求文档和断言把线程边界写清楚。

最终判断一句话：

> Tudou 当前的二层哈希不需要加锁；但这个结论依赖“外层只读、内层 loop 独占、配置 start 前完成、对象生命周期覆盖回调”这四个前提。违反任意一个前提，就不能再用 muduo 或当前 Tudou 的无锁结论来解释安全性。
