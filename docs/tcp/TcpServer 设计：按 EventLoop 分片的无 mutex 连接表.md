# TcpServer 设计：按 EventLoop 分片的无 mutex 连接表

Tudou 的连接表不在 `unordered_map` 外加互斥锁，而是按 owner EventLoop 分片。准确说法是“连接表不使用 mutex”，不是“整个 TcpServer 形式化 lock-free”。

# Situation — 情境

多 IO 线程服务器需要记录活跃连接，以便关闭、统计和停止时统一收口。若所有线程都读写同一张 `unordered_map`，就必须为每次插入、删除和查找加锁；连接建立与关闭频繁时，这会把锁放到热点路径。

`std::unordered_map` 本身不支持并发读写。不能因为“不同线程写的是不同 key”就认为共享 map 安全。

# Task — 任务

连接表设计需要：

1. 新连接在所属 IO 线程登记，关闭时在同一线程删除；
2. 多个 IO 线程之间不竞争同一个内层容器；
3. 停止阶段能够拒绝新连接、收口旧连接并安全销毁目录；
4. 明确哪些状态仍需要 atomic、mutex 和 condition variable；
5. 让面试时能严格证明“为什么没有连接表锁也安全”。

# Action — 设计与实现

## 两层连接表与所有权

```cpp
std::unordered_map<
    EventLoop*,
    std::unordered_map<TcpConnection*, TcpConnectionPtr>
> connectionRecordsByLoop_;
```

```text
外层目录：EventLoop* → 该 loop 的本地连接表
内层账本：TcpConnection* → owning TcpConnectionPtr
```

key 使用 `TcpConnection*` 只表示稳定对象身份；value 才持有连接所有权。fd 可能在连接关闭后被系统复用，不适合作为连接身份；用 `shared_ptr` 作 key 则会额外参与所有权和哈希语义，反而增加模型复杂度。

## 启动阶段：一次性构建外层目录

线程池启动后、开始接受连接前，TcpServer 收集所有 EventLoop 并创建每个分片：

```cpp
const auto loops = loopThreadPool_->get_all_loops();
connectionRecordsByLoop_.reserve(loops.size());
for (EventLoop* loop : loops) {
    connectionRecordsByLoop_.emplace(
        loop, std::unordered_map<TcpConnection*, TcpConnectionPtr>());
}
accepting_.store(true);
```

此后运行期不再对外层 map `emplace`、`erase`、`clear` 或触发 rehash；外层 map 只充当只读目录。

## 运行阶段：内层表由 owner loop 独占

新连接在 main loop accept 后，先选定目标 IO loop，再投递创建任务：

```text
main loop accept
  → get_next_loop() 选择 owner loop
  → ownerLoop.run_in_loop(...)
  → create_connection() 插入 owner 的 localRecords

owner loop 收到关闭事件
  → on_close()
  → remove_connection()
  → 从同一 localRecords 删除
```

`create_connection()` 与 `remove_connection()` 都断言当前处于 owner loop。每个内层 `unordered_map` 只被一个线程访问，因此不同 IO 线程修改的是不同容器对象，不存在同一容器的并发读写。

```cpp
auto recordsIt = connectionRecordsByLoop_.find(&ioLoop);
auto& localRecords = recordsIt->second;

localRecords[conn.get()] = conn;
activeConnectionCount_.fetch_add(1);
```

## 停止阶段：先冻结入口，再按分片收口

```text
stop()
  → accepting_ = false，拒绝新连接
  → main loop 退出
  → 向每个 owner loop 投递收口任务
  → owner loop 摘出自己的 localRecords 并 force_close
  → activeConnectionCount_ 归零
  → join IO 线程
  → clear 外层目录
```

收口任务只在所属 loop 中触碰内层 map。`activeConnectionCount_` 是 atomic；`shutdownMutex_` 仅服务 condition variable 等待，不保护连接表。线程池 join 完成前不会 clear 外层 map，因此不会出现 IO 线程仍在 `find()` 时主线程销毁目录。

## 线程安全边界

| 状态 | 同步方式 |
| :--- | :--- |
| 外层连接目录 | 启动后只读，join 后才清理 |
| 每个内层连接表 | 对应 owner EventLoop 独占 |
| 跨线程连接操作 | 投递到 owner loop |
| 活跃连接计数 | atomic |
| shutdown 等待 | condition variable + 配套 mutex |
| 回调、高水位、心跳配置 | 应在 `start()` 前设置 |

配置字段没有运行期同步；在服务器运行时并发调用 `set_*` 会与 IO 线程读取形成数据竞争。连接表无 mutex 不等于 TcpServer 的所有接口可随意并发调用。

# Result — 结果

- 连接创建、处理、删除都在同一个 owner loop 形成闭环；
- IO 线程没有集中连接表锁竞争；
- 外层目录结构稳定，运行期可以并发只读；
- 停止顺序保证目录销毁不会与 IO 线程访问并发；
- 数据结构、所有权和线程归属能直接对应源码断言与生命周期。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 分片方式 | 以 EventLoop 为分片单位，而非以 fd 或哈希分片。 |
| 外层 map | 启动时构建，运行期只读，线程池 join 后清理。 |
| 内层 map | 只由对应 owner loop 插入、查找、删除。 |
| key / value | 裸指针表示身份，`TcpConnectionPtr` 表示所有权。 |
| 停止顺序 | 先拒绝新连接，按 owner 收口，再 join，最后 clear。 |
| 准确表述 | 无连接表 mutex，不是整个服务无锁。 |

# 面试核心问答总结

## Q1：为什么这个 unordered_map 不需要 mutex？

不是一个 map 被多线程并发写。外层 map 启动后只读；每个内层 map 只属于一个 EventLoop 线程。连接插入和删除都回到 owner loop，因此同一个容器没有并发读写。

## Q2：为什么外层 map 可以被多个线程 find？

因为运行期没有任何线程修改外层结构、插入元素或 rehash，只有并发只读。清理发生在线程池 join 之后，IO 线程已经退出。

## Q3：为什么 key 不使用 fd？

fd 会被操作系统复用：旧连接关闭后，新连接可能得到同一个整数。`TcpConnection*` 在记录存在期间稳定，value 中的 shared_ptr 又确保该对象存活。

## Q4：这是不是 lock-free？

不是。准确说连接表不使用 mutex；pending functors、shutdown 等待仍使用 mutex、atomic 和 condition variable。安全来自线程归属，而不是 STL 容器具备并发能力。
