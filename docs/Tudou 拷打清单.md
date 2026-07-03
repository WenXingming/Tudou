# Tudou 面试拷打清单

> 这份文档用于临场模拟面试，不是完整题库。
> 目标是逼自己讲清楚项目的核心路径、设计取舍、实现细节和可改进点。

---

## 使用方式

每个问题先口述 60 到 120 秒，不看代码、不看参考答案。答完后用下面的“合格边界”和“追问点”自查。

如果一个问题只能背概念，不能说到本项目中的具体类、具体函数、具体数据结构，就视为没过。

---

## 一分钟项目叙述

### Q1：你用一分钟介绍一下 Tudou。不要背 README，说清楚你到底做了什么。

**合格边界：**

Tudou 是一个面向 Linux 的 C++14 多线程 Reactor 网络框架。底层用 `epoll` 做 I/O 多路复用，用 `eventfd` 做跨线程唤醒，用 `timerfd` 把定时任务统一进事件循环。核心模块包括 `EventLoop`、`EpollPoller`、`Channel`、`Acceptor`、`TcpConnection`、`TcpServer`、`Buffer`、`TimerQueue`，上层封装了 HTTP/HTTPS、路由和连接空闲检测，并提供静态文件服务、文件分享服务、聊天服务等示例。

重点不要只说“高性能网络框架”。要讲清楚你承担的是基础设施层工作：连接接入、事件分发、跨线程投递、连接生命周期、协议解析和服务端门面。

**追问点：**

- 为什么它是 Reactor，不是线程池处理每个连接？
- `one loop per thread` 给你带来了什么？代价是什么？
- 哪些能力是框架能力，哪些只是 example 应用？

---

## 从一次请求讲全链路

### Q2：一个 HTTP 请求从客户端发来，到响应写回去，完整调用链是什么？

**合格边界：**

能按下面路径讲清楚：

`listen fd` 可读 -> `Acceptor::on_read()` -> `Socket::accept()` 得到连接 fd -> `TcpServer::on_connect()` 选择一个 IO loop -> 在 IO loop 中 `TcpConnection::create_connection()` -> 连接 `Channel` 监听读事件 -> `TcpConnection::on_read()` -> `Buffer::read_from_fd()` -> `TcpServer::on_message()` -> `HttpServer::on_message()` -> `conn->receive()` 取出字节 -> `HttpContext::parse()` 调 llhttp -> `Router::dispatch()` -> `HttpResponse::package_to_string()` -> `conn->send()` -> `TcpConnection::send_in_loop()` 直接写或进入写缓冲 -> `EPOLLOUT` 触发 `on_write()` 继续发送。

**追问点：**

- 新连接为什么要投递到 IO loop 里创建 `TcpConnection`？
- 为什么 `conn->receive()` 是在 HTTP 层调用，而不是 TCP 层自动把字符串传上去？
- 如果请求只到了一半，`HttpContext` 怎么处理？

---

## Reactor 核心

### Q3：`EventLoop::loop()` 一轮循环做了什么？

**合格边界：**

一轮循环包括三步：`poller_->poll()` 调 `epoll_wait` 拿活跃 `Channel`；遍历活跃 `Channel` 调 `handle_events()`；最后 `do_pending_functors()` 执行跨线程投递过来的任务。退出依赖 `isQuit_`。

要特别指出：`eventfd` 不是业务 fd，而是唤醒 fd。其他线程 `queue_in_loop()` 后写 `eventfd`，让阻塞中的 `epoll_wait` 立即返回。

**追问点：**

- 为什么 `queue_in_loop()` 要在跨线程时唤醒？
- 当前实现里，正在执行 pending functors 时再次投递任务也会唤醒，为什么？
- `do_pending_functors()` 为什么先把队列 swap 到局部变量？

### Q4：`run_in_loop()` 和 `queue_in_loop()` 区别是什么？

**合格边界：**

`run_in_loop()` 是“如果当前就在 loop 线程，立即执行；否则入队”。`queue_in_loop()` 是“总是入队，必要时唤醒”。这两个接口的意义是区分同步执行和异步投递，避免同线程场景产生不必要的队列和系统调用开销。

**追问点：**

- 如果在 IO 线程外直接操作 `TcpConnection` 会有什么问题？
- 为什么任务队列需要锁，而连接表可以做到无锁？

---

## Channel 与生命周期

### Q5：`Channel` 到底管理 fd 吗？它析构时做什么？

**合格边界：**

不能说 `Channel` 关闭 fd。当前实现中，fd 的所有权由 `Socket` 管理，`Socket` 析构时关闭 fd。`Channel` 只管理这个 fd 在 `epoll` 里的事件兴趣和回调分发。`Channel::~Channel()` 会 `disable_all()`，再 `remove_in_register()`，确保 `epoll` 不再持有指向这个 `Channel` 的指针。

`TcpConnection` 中 `connSocket_` 声明在 `channel_` 之前，因此析构逆序是 `channel_` 先析构、从 epoll 注销，然后 `connSocket_` 再关闭 fd。

**追问点：**

- 如果先 close fd，再从 epoll 删除，会有什么风险？
- 为什么 `Channel` 构造时就注册到 `Poller`？
- 为什么 `Channel` 析构必须发生在所属 `EventLoop` 线程？

### Q6：`Channel::tie_to_object()` 解决什么问题？

**合格边界：**

`tie_to_object()` 解决回调执行期间 owner 被释放的问题。`TcpConnection` 用 `shared_ptr` 管理，但 `Channel` 的回调捕获的是 `this`。如果回调链中关闭连接，可能导致 `TcpConnection` 析构。`Channel::handle_events()` 先把 `weak_ptr` 升级成临时 `shared_ptr`，保证本轮回调执行期间连接对象活着。

**追问点：**

- 为什么 tie 用 `weak_ptr`，不是 `shared_ptr`？
- 为什么 `Acceptor` 的 `Channel` 不需要 tie？
- tie 能不能防止所有生命周期 bug？不能，它只保证回调期间 owner 不析构。

---

## TCP 连接与写缓冲

### Q7：`TcpConnection::send()` 在不可写时怎么处理？

**合格边界：**

如果调用线程不是连接所属 loop，先通过 `queue_in_loop()` 投递回正确线程。`send_in_loop()` 先尝试直接 `write`，如果一次写完就触发写完成回调。如果只写了一部分或者暂时不可写，就把剩余数据放入 `writeBuffer_`，再 `enable_writing()` 关注 `EPOLLOUT`。后续 fd 可写时 `on_write()` 刷缓冲，清空后关闭写事件关注。

还要提到高水位：当写缓冲从低于阈值变为超过阈值时触发 `highWaterMarkCallback_`，上层可以做背压控制。

**追问点：**

- 为什么不能一直关注 `EPOLLOUT`？
- 高水位回调为什么只在跨过阈值那一次触发？
- 当前实现的 `write_to_fd()` 用一次 `write`，如果还有剩余怎么办？

### Q8：`Buffer::read_from_fd()` 为什么用 `readv`？

**合格边界：**

`readv` 同时给两个 iovec：第一段是 `Buffer` 当前 writable 区，第二段是栈上 `extraBuf`。如果内置空间够，一次读入 Buffer；如果不够，溢出的数据进栈缓冲，再追加回 Buffer。这样避免每次读之前都先扩容，也减少系统调用次数。

**追问点：**

- 为什么 `make_space()` 中搬移数据要用 `memmove`？
- prepend 区有什么用？
- 水平触发模式下，读回调不消费数据会怎样？

---

## TcpServer 并发模型

### Q9：`TcpServer` 的连接管理为什么可以无锁？

**合格边界：**

连接记录是两层结构：外层 `unordered_map<EventLoop*, ConnectionRecords>` 在 `start()` 阶段一次性初始化，运行期只读；内层 `ConnectionRecords` 只允许所属 `EventLoop` 线程增删改查。每条连接的读、写、关闭、心跳刷新都投递到同一个 loop，因此不需要锁保护连接表。

要区分：无锁的是连接记录的运行期增删查；跨线程任务队列仍然需要互斥锁，因为多个线程可以同时投递任务。

**追问点：**

- 外层 map 如果运行期插入会发生什么？
- `activeConnectionCount_` 为什么是 atomic？
- `shutdownMutex_` 保护的是什么，不保护什么？

### Q10：`TcpServer::stop()` 后如何收口已有连接？

**合格边界：**

`stop()` 把状态从 `Running` 切到 `Draining`，再让 main loop `quit()`。`start()` 中的 `mainLoop.loop()` 返回后调用 `shutdown_connections()`。它向每个 loop 投递清理任务，把本 loop 的连接记录摘出来，停止心跳、清掉部分回调并 `force_close()`，同时通过 `activeConnectionCount_` 和条件变量等待连接清理完成，最后销毁 `acceptor_` 和线程池。

**追问点：**

- 为什么 stop 不是直接析构线程池？
- Draining 状态下新 accept 到的连接怎么处理？
- 为什么清理连接要投递到连接所属 loop？

---

## 定时器与心跳

### Q11：`TimerQueue` 为什么用 `timerfd`？

**合格边界：**

因为 `timerfd` 能把“时间到期”变成 fd 可读事件，统一交给 `epoll` 和 `Channel` 处理。不需要信号处理函数，不需要额外定时线程，定时器回调天然在所属 `EventLoop` 线程执行。

**追问点：**

- 为什么用 `CLOCK_MONOTONIC`？
- `read_timerfd()` 不读会怎样？
- `timerfd_settime()` 设置的是相对时间还是绝对时间？当前实现传 flag 0，所以是相对时间。

### Q12：`TimerQueue` 的双索引和懒删除怎么工作？

**合格边界：**

`expireHeap_` 是按到期时间排序的最小堆，负责快速找到最早到期定时器。`timersById_` 是按 `TimerId` 查找的 map，也是 `Timer` 对象的真实持有者。取消定时器时只从 `timersById_` 删除，堆里的旧条目等到堆顶检查时发现不存在再丢弃，这就是懒删除。

**追问点：**

- 为什么 `priority_queue` 不能直接删除中间元素？
- 回调中取消自己，当前代码如何避免继续重插？
- 当前实现的一个可改进点：如果取消的是堆顶最早定时器，`sync_timerfd()` 可能会按已取消的旧堆顶重新武装 timerfd，造成一次多余唤醒；可以在 `sync_timerfd()` 前清理堆顶失效条目。

### Q13：连接心跳为什么用 `weak_ptr<TcpConnection>`？

**合格边界：**

心跳对象不应该拥有连接，否则 `ConnectionRecord` 持有连接和心跳，心跳再强持有连接，会形成引用环。用 `weak_ptr` 后，连接关闭时心跳回调 `lock()` 失败，就可以停止检测。

**追问点：**

- `refresh()` 在哪里调用？当前是 `TcpServer::on_message()` 中刷新。
- 空闲检测是读空闲，还是读写空闲？当前主要是读活跃刷新。
- 如果业务层长时间只发送不接收，会不会被判空闲？

---

## Acceptor 与 fd 耗尽

### Q14：fd 耗尽时为什么会 busy-loop？Tudou 怎么处理？

**合格边界：**

当进程 fd 达到上限时，`accept` 会返回 `EMFILE` 或 `ENFILE`。但内核 accept 队列里仍有完成握手的连接，监听 fd 仍然可读，水平触发 epoll 会反复通知，形成 busy-loop。

当前 `Acceptor` 预留一个 `idleFd_`。fd 耗尽时先关闭它腾出一个 fd 名额，再 `accept` 拉走一个挂起连接，把这个连接 fd 接管为新的 `idleFd_`，从而让 accept 队列向前推进，避免监听 fd 一直可读。

**追问点：**

- 这和“关闭一个已有业务连接”相比有什么优势？
- 为什么这里不是简单 sleep 后重试？
- 当前实现和经典“accept 后立即 close，再重新 open /dev/null”略有不同，面试时要按代码讲。

---

## HTTP 与路由

### Q15：为什么选择 llhttp，而不是自己手写 HTTP parser？

**合格边界：**

HTTP 解析边界多，分片输入、header 分段、body、错误请求、版本、方法、URL/query 都需要状态机处理。llhttp 是成熟的 C 状态机解析库，适合做底层 parser。Tudou 的 `HttpContext` 把 llhttp 的静态回调桥接回对象，把解析过程收敛成 `HttpRequest`，并用 `NeedMoreData / Complete / Rejected` 三态返回给 `HttpServer`。

**追问点：**

- header field/value 分片时怎么保证不丢？
- `HttpContext::reset()` 为什么要同时清 DTO 和 llhttp 状态机？
- 当前 HTTP 层是否支持流水线请求？如果没做完整队列化处理，不要夸大。

### Q16：`Router::dispatch()` 的匹配顺序为什么是精确、405、前缀、404？

**合格边界：**

精确路由优先，避免兜底吞掉明确业务入口。同一路径存在但方法不匹配时，应返回 405，并设置 `Allow`。只有路径不属于精确路由表时，前缀路由才接管。最后都不匹配才 404。

**追问点：**

- 为什么 prefix route 保留注册顺序？
- `allowedMethodsByPath_` 的作用是什么？
- 如果 `/api` 是前缀路由，`POST /users` 已存在但 `GET /users` 不存在，应该走 405 还是前缀？

---

## TLS

### Q17：HTTPS 是怎么接入到原来的 TCP/HTTP 流程里的？

**合格边界：**

`HttpServer::enable_ssl()` 初始化 `SslContext`。每条连接建立时创建一个 `TlsConnection` 存在 `ConnectionState` 中。收到 TCP 字节后，如果是 TLS 连接，先用 `TlsConnection::read_plaintext()` 解密出明文 HTTP 数据，同时可能产生握手阶段要发回的密文；响应发送前通过 `write_plaintext()` 加密，再交给 `TcpConnection::send()`。

重点是：TCP 层仍然只处理字节流，HTTP 业务层仍然操作明文请求和响应，TLS 被夹在二者之间作为连接状态的一部分。

**追问点：**

- 为什么 TLS 状态是每连接一个？
- TLS 握手没完成时，`HttpServer::on_message()` 为什么不能继续解析 HTTP？
- `connectionStates_` 为什么需要 mutex？

---

## 性能与取舍

### Q18：你的性能数据能说明什么？不能说明什么？

**合格边界：**

可以说 Tudou 在本地 hello benchmark 中已经和 muduo 同数量级，多线程下吞吐接近百万 QPS，说明 Reactor 主干、连接分发和 HTTP 封装没有明显拖垮性能。

但不能夸大：这些是 localhost、内存响应、短时间 wrk 压测，不等价于公网真实负载，也不能证明文件服务、TLS、大包、慢客户端场景都高性能。面试时要主动说明边界，反而更可信。

**追问点：**

- 为什么多线程性能提升不是线性？
- wrk 的线程数、连接数、持续时间分别影响什么？
- 如果要压测静态文件服务，指标和 hello benchmark 有什么不同？

### Q19：如果继续优化，你会优先改哪三件事？

**合格边界：**

可以选三类，不要贪多：

1. 静态文件服务引入 `sendfile` 或更完整的零拷贝路径，减少用户态拷贝。
2. 接入 `SO_REUSEPORT` 或多 acceptor，缓解单 main loop accept 瓶颈。
3. 连接分配从 round-robin 改成按 loop 负载分配，比如最少连接或队列长度。
4. `TimerQueue::sync_timerfd()` 清理堆顶失效定时器，减少取消最早定时器后的多余唤醒。
5. Buffer/连接对象做内存池，降低大量连接时的分配和碎片成本。

**追问点：**

- 哪个优化对当前 benchmark 最可能有效？
- 哪个优化会增加复杂度但收益不确定？
- 你怎么验证优化真的有效？

---

## 项目复盘

### Q20：这个项目你最能体现工程能力的地方是什么？

**合格边界：**

不要泛泛说“实现了 Reactor”。更有说服力的说法是：

- 生命周期处理：`Socket` 管 fd、`Channel` 管 epoll 注册、`TcpConnection` 用 `shared_ptr`、`Channel` 用 weak tie 保活，析构顺序明确。
- 线程模型：one loop per thread，跨线程操作统一投递，连接表按 loop 分片，无锁但边界清楚。
- Linux fd 事件统一：socket、eventfd、timerfd 都进入 epoll，形成统一事件分发模型。
- 协议层封装：TCP 层不理解 HTTP，HTTP 层通过 `HttpContext` 和 `Router` 做解析和分发，TLS 作为连接状态夹在 TCP 和 HTTP 之间。

**追问点：**

- 你最容易被问倒的点是什么？
- 哪个实现你现在看会重构？
- 如果面试官质疑“这不就是 muduo 仿写”，你怎么回答？

---

## 最容易说错的点

- `Channel` 不关闭 fd；`Socket` 才持有并关闭 fd。
- `queue_in_loop()` 的任务队列需要锁；连接表无锁不代表项目完全无锁。
- `eventfd` 负责跨线程唤醒，不负责传递业务数据。
- `timerfd_settime(..., 0, ...)` 使用相对时间；当前代码传的是 duration 转出来的 `timespec`。
- `ConnectionHeartbeat` 当前主要根据读消息刷新活跃时间，不要说成完整 TCP keepalive 替代品。
- `Acceptor` 的 idle fd 实现要按当前代码讲：fd 耗尽后接受一个挂起连接并接管为新的 idle fd。
- benchmark 只能说明特定场景下的吞吐和延迟，不能泛化成所有生产场景。

