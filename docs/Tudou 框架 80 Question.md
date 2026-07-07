# Tudou 框架 80 问

> 覆盖项目每一个类、每一个设计决策、每一个实现细节。
> 按模块分组，每题附口述参考答案。

---

## 一、架构与设计模式（Q1–Q5）

### Q1：Tudou 的分层架构是什么？各层的职责边界在哪？

**口述答案：**

四层架构，从底往上：

**Reactor 层**（EventLoop + EpollPoller + Channel）：负责事件循环和 I/O 多路复用，把 fd 的可读/可写事件转化为回调调用。

**定时器层**（Timer + TimerQueue）：基于 Linux timerfd，把"时间到期"转化为 epoll 可读事件，复用 Reactor 的事件循环。

**TCP 层**（Acceptor + TcpConnection + TcpServer + Buffer + Socket）：负责连接管理、数据收发、连接生命周期编排。

**HTTP 层**（HttpContext + HttpRequest + HttpResponse + Router + HttpServer）：负责 HTTP 协议解析、路由分发、请求-响应流程。

层与层之间通过回调（`std::function`）和 `shared_ptr` 解耦，上层持有下层的回调注册权，但不直接操控下层内部状态。

---

### Q2：项目中用了哪些设计模式？各体现在哪里？

**口述答案：**

**Reactor 模式**：EventLoop 是核心 Reactor，EpollPoller 是 Demultiplexer，Channel 是 Handler。`epoll_wait` 返回就绪事件后，通过 Channel 分发到具体回调。

**门面模式**：TcpServer 是 TCP 层的门面，对外只暴露 `start/stop` 和回调注册接口，隐藏了 Acceptor、线程池、连接表等所有内部编排。HttpServer 同理，对外只暴露 `add_route/start`。

**工厂方法**：`TcpConnection::create_connection` 是工厂方法，因为 `enable_shared_from_this` 要求对象必须由 `shared_ptr` 管理，构造分两步（先 new + shared_ptr，再 tie + enable_reading），工厂封装了这个顺序。

**RAII**：Socket 用 RAII 管理 fd 生命周期，析构时自动 `::close(fd)`。移动语义保证 fd 所有权唯一。

**观察者模式**：回调机制本质上就是观察者——`set_read_callback`、`set_message_callback` 等注册接口让上层订阅下层事件。

**依赖注入**：`TimerQueue(EventLoop* loop)` 通过构造函数注入所属 EventLoop，不持有所有权。

---

### Q3：为什么 Socket 的 fd 用 `int` 而不是 `FILE*` 或 `std::fstream`？

**口述答案：**

在 Linux 网络编程中，socket fd 就是一个整数，由内核管理。`FILE*` 和 `std::fstream` 是 C/C++ 标准库对文件 I/O 的缓冲封装，它们在底层也是用 `int fd` 实现的，但多了一层用户态缓冲。

网络编程需要精确控制读写行为：非阻塞 I/O、`readv/writev` 分散聚合 I/O、`epoll` 事件通知等，这些都要求直接操作 fd。用 `FILE*` 会引入额外的缓冲层，导致 `epoll` 通知可读时，数据可能已经被 `FILE*` 的内部缓冲消费了，行为不可预测。

所以网络框架普遍直接用 `int fd`，配合 `read/write/readv/writev/epoll_ctl` 等系统调用。

---

### Q4：项目中哪些地方用了 `shared_ptr`，哪些地方用了 `unique_ptr`？选择依据是什么？

**口述答案：**

`shared_ptr` 用于**跨层共享所有权**的场景：
- `TcpConnection` 用 `shared_ptr`（`TcpConnectionPtr`），因为连接的生命周期跨越 TcpServer、EventLoop、Channel 多个组件，且用户回调也可能持有连接引用。`enable_shared_from_this` 让连接能安全地把自身 `shared_ptr` 传给回调。
- `ConnectionHeartbeat` 用 `shared_ptr`，因为它附着在连接上，需要和连接共享生命周期。
- `Timer` 用 `shared_ptr`，因为 `timersById_` 和 `expireHeap_`（懒删除）需要共享访问。

`unique_ptr` 用于**单一所有权**的场景：
- `Channel` 用 `unique_ptr`，每个 Channel 只被一个 owner 持有（TcpConnection 或 Acceptor 或 TimerQueue）。
- `Buffer` 用 `unique_ptr`，每个连接有独立的读写缓冲。
- `EventLoopThreadPool`、`Acceptor`、`SslContext` 等内部组件用 `unique_ptr`，表达"创建和销毁由 owner 负责"。

**选择依据**：如果对象的生命周期需要多个独立组件共同管理，用 `shared_ptr`；如果只有一个明确的 owner，用 `unique_ptr`。`unique_ptr` 是零开销的，优先使用。

---

### Q5：为什么很多类的构造函数接收 `EventLoop*` 裸指针而不是 `shared_ptr<EventLoop>`？

**口述答案：**

因为 EventLoop 的生命周期由外部（TcpServer 或 EventLoopThread）明确管理，这些类（Channel、TimerQueue、Acceptor 等）只是"使用" EventLoop，不"拥有"它。

用裸指针表达的是"非所有权的访问"——我不负责你的生死，我只是借用你的接口。如果用 `shared_ptr`，语义上变成了"共享所有权"，但实际上 Channel 的生命周期一定短于 EventLoop（Channel 析构时要调用 `loop_->remove_channel`），用 `shared_ptr` 反而可能延长 EventLoop 的生命周期，导致析构顺序错乱。

这是项目的一条规范："生命周期由智能指针管理，访问用裸指针"。`assert_in_loop_thread()` 守卫保证了不会在 EventLoop 已析构后还访问它。

---

## 二、EventLoop（Q6–Q12）

### Q6：EventLoop 构造时做了什么？为什么需要 eventfd？

**口述答案：**

EventLoop 构造时创建四样东西：

1. **EpollPoller**：底层 epoll 实例。
2. **wakeupFd_**（eventfd）：用于跨线程唤醒。当其他线程通过 `run_in_loop` 投递任务时，需要唤醒阻塞在 `epoll_wait` 中的 EventLoop。往 eventfd 写一个字节，epoll_wait 就会返回。
3. **wakeupChannel_**：监听 wakeupFd_ 的 Channel，读回调是 `on_read()`，负责消费 eventfd 中的事件。
4. **TimerQueue**：定时器队列。

为什么需要 eventfd？因为 `epoll_wait` 是阻塞调用，如果 EventLoop 线程正在等待事件，其他线程投递的任务要等到 `epoll_wait` 超时才能执行。eventfd 提供了一个零延迟唤醒机制——往里写一个字节，`epoll_wait` 立即返回，任务马上执行。

---

### Q7：`do_pending_functors` 为什么要"摘队列"而不是逐个 pop？

**口述答案：**

"摘队列"是指把 `pendingFunctors_` 整个 swap 到一个局部变量，然后在局部变量上逐个执行。

这样做是为了**减少锁持有时间**。`pendingFunctors_` 被 `pendingFunctorsMutex_` 保护，如果逐个 pop，锁要一直持有到所有任务执行完。而 swap 只需要一瞬间——拿到锁，swap 一下，松开锁，然后在局部变量上慢慢执行。执行期间其他线程可以继续往 `pendingFunctors_` 投递新任务，不会被阻塞。

另外，swap 还避免了"重入投递"的问题——如果某个 functor 执行时又调用了 `queue_in_loop`，新任务会进入已经被 swap 走的空队列，不会和正在执行的这批任务混在一起，避免了无限递归。

---

### Q8：`isCallingPendingFunctors_` 这个标志是做什么的？

**口述答案：**

这个标志解决的是**重入投递时的唤醒优化**。

场景：EventLoop 线程正在执行 `do_pending_functors`，其中一个 functor 又调用了 `queue_in_loop`。这时候 `queue_in_loop` 发现当前已经在 EventLoop 线程了（`is_in_loop_thread()` 为 true），但不是在 poll 阶段（是在执行 pending functors 阶段）。

如果不做特殊处理，`queue_in_loop` 会无条件调用 `wakeup()` 写 eventfd。但这其实是不必要的——因为 `do_pending_functors` 执行完后，下一轮循环会再次 poll，新投递的任务自然会被处理。

所以 `isCallingPendingFunctors_` 为 true 时，`queue_in_loop` 可以跳过 `wakeup()` 调用，减少一次 eventfd write 系统调用。但为了保证及时性，某些场景下仍然需要唤醒（比如有更高优先级的任务），所以这个优化是可选的。

---

### Q9：EventLoop 的 `quit()` 是怎么保证线程安全的？

**口述答案：**

`quit()` 设置 `isQuit_` 为 true，这是个 `atomic<bool>`，所以多线程写入是安全的。

但还有一个细节：如果 `quit()` 是从非 EventLoop 线程调用的，EventLoop 可能正阻塞在 `epoll_wait` 中，光设置标志不够，还需要唤醒它。所以 `quit()` 内部会检查 `is_in_loop_thread()`，如果不是当前线程就调用 `wakeup()`，让 `epoll_wait` 立即返回，下一轮循环检查到 `isQuit_` 就退出。

---

### Q10：`loop()` 里 `activeChannels_` 是成员变量而不是局部变量，为什么？

**口述答案：**

性能优化。如果 `activeChannels_` 是局部变量，每轮循环都要构造和析构一个 `vector<Channel*>`，涉及堆分配和释放。改为成员变量后，`vector` 在多轮循环中复用，只需要 `clear()` 清空内容，底层内存不释放，下一轮直接复用。

对于高频事件循环来说，每轮省一次堆分配，累积效果很明显。`clear()` 只重置 size 不释放 capacity，是 O(1) 操作。

---

### Q11：EventLoop 析构时做了什么？为什么要做线程归属检查？

**口述答案：**

EventLoop 析构时首先断言当前线程就是 EventLoop 所属线程（`assert_in_loop_thread()`），然后将 `loopInThisThread`（thread_local）设为 nullptr，表示当前线程不再有活跃的 EventLoop。

之后成员变量按声明逆序自动析构：timerQueue_ → pendingFunctorsMutex_ → wakeupChannel_ → wakeupFd_ → poller_。wakeupChannel_ 声明在 wakeupFd_ 之前，保证 Channel 先注销 epoll 再关闭 eventfd。

线程归属检查的原因：如果 EventLoop 在非所属线程被析构，说明有其他线程还在持有它的引用（比如通过 `run_in_loop`），此时析构会导致 use-after-free。断言失败提前暴露 bug。

---

### Q12：EventLoop 的 poll 超时时间是怎么选择的？

**口述答案：**

默认是 10 秒（`pollTimeoutMs_ = 10000`）。这个值是个权衡：

太短（比如 1ms）会导致 epoll_wait 频繁返回（即使没有事件），浪费 CPU。太长（比如 60 秒）会导致定时器和跨线程任务的延迟——如果最近的定时器在 1 秒后到期，但 epoll_wait 阻塞了 60 秒，那定时器就要延迟 59 秒才执行。

实际上定时器的到期时间由 timerfd 精确控制，timerfd 注册在 epoll 中，到期时 epoll_wait 会立即返回。所以 poll 超时主要是兜底——没有定时器、没有 I/O 事件时，最多等 10 秒就回去检查一下 pending functors 和 quit 标志。

测试中用 20ms 是为了加快测试速度。

---

## 三、Channel（Q13–Q18）

### Q13：Channel 的构造函数为什么要做 `update_in_register()`？

**口述答案：**

Channel 构造时 `events_` 是 `kNoneEvent_`（0），`update_in_register()` 把这个"无事件兴趣"状态同步到 Poller。这相当于告诉 epoll："我有这个 fd 了，但现在不关注任何事件。"

这是一种**防御式设计**：Channel 一旦构造就立即纳入 Poller 管理，保证生命周期内 Channel 和 Poller 始终同步。如果构造时不做注册，那后续 `enable_reading()` 时的 `epoll_ctl` 就不知道该用 ADD 还是 MOD（因为 fd 还没注册到 epoll）。

析构时的 `remove_in_register()` 做对称的反注册，保证 Channel 销毁后 Poller 不再持有任何引用。

---

### Q14：事件分发的优先级为什么是"关闭 > 错误 > 读 > 写"？

**口述答案：**

这是一个**安全优先**的设计。

**关闭优先于读**：EPOLLHUP 单独到来时走关闭回调。但 EPOLLHUP + EPOLLIN 同时到来时（常见于 read 返回 0 即 EOF 场景），走读回调更合理——让应用层读到 EOF，做优雅关闭。所以代码里是 `if (EPOLLHUP && !EPOLLIN) → close`。

**错误优先于读写**：EPOLLERR 表示 fd 出错了（比如连接重置），此时再读写没有意义，应该先通知错误回调，让上层决定怎么处理。

**读优先于写**：大多数场景下，读事件比写事件更重要（响应请求比发送数据更紧迫）。写事件通常是"有数据要发"才注册的，读事件是持续监听的。

---

### Q15：`set_read_callback` 是必选的，`set_write_callback` 是可选的。为什么？

**口述答案：**

读回调是**必选**的，因为：
- Channel 注册了 EPOLLIN 就意味着有数据要处理，如果没有读回调就断言失败。
- 网络编程的核心模式就是"有数据来就处理"，读回调是最基本的事件。

写回调是**可选**的，因为：
- 写事件（EPOLLOUT）不是持续监听的，只在"有数据要发但发送缓冲区满"时才临时注册。
- 很多 Channel 只需要监听读事件（比如监听 socket、只接收数据的连接）。
- `handle_write_callback` 里有 `if (!writeCallback_) return;` 保护，未设置时直接跳过。

---

### Q16：Channel 的 `disable_all()` 和析构时的 `remove_in_register()` 有什么区别？

**口述答案：**

`disable_all()` 把 `events_` 设为 `kNoneEvent_`，然后 `epoll_ctl(MOD)` 更新——fd 还在 epoll 里，只是不关注任何事件了。这相当于"暂停监听"，之后可以再 `enable_reading()` 恢复。

`remove_in_register()` 调用 `epoll_ctl(DEL)`——fd 从 epoll 中彻底移除。这相当于"注销"，之后 fd 不再受 Poller 管理。

析构时先 `disable_all()` 再 `remove_in_register()`：先把事件清零（MOD），再彻底删除（DEL）。两步都做是为了保证状态一致性——即使 DEL 失败（比如 fd 已被关闭），至少 events_ 已经是 0 了。

---

### Q17：Channel 持有的 `loop_` 指针会在 Channel 生命周期内变成悬空吗？

**口述答案：**

不会。因为 Channel 的生命周期一定短于 EventLoop：

- Channel 由 TcpConnection、Acceptor 或 TimerQueue 持有（`unique_ptr<Channel>`）。
- 这些 owner 的析构顺序保证了 Channel 先析构、EventLoop 后析构。
- 具体来说：TcpConnection 的 `connSocket_` 声明在 `channel_` 之前，析构时 `channel_` 先销毁（调用 `remove_in_register`），`connSocket_` 后销毁。EventLoop 的成员也按声明逆序析构。

所以 Channel 析构时 `loop_` 一定是有效的。`assert_in_loop_thread()` 在析构时进一步验证了这一点。

---

### Q18：`tie_to_object` 的参数是 `shared_ptr<void>`，为什么不是模板？

**口述答案：**

因为 `tie_to_object` 不关心 owner 的具体类型，它只需要一个 `shared_ptr` 来做引用计数。`shared_ptr<void>` 是类型擦除的——任何 `shared_ptr<T>` 都可以隐式转换为 `shared_ptr<void>`，因为 `shared_ptr` 的控制块保存了类型擦除的删除器。

实际使用时，调用方传入 `shared_ptr<TcpConnection>`，隐式转换为 `shared_ptr<void>`。内部只做 `weak_ptr<void>` 的 `lock()` 和 `expired()` 检查，不访问对象的具体内容，所以不需要知道类型。

这比模板更简洁——如果用模板 `template<typename T> void tie_to_object(const shared_ptr<T>&)`，每个 T 都会生成一份代码，而 `shared_ptr<void>` 只有一份。

---

## 四、EpollPoller（Q19–Q23）

### Q19：EpollPoller 的 `poll()` 方法做了哪三件事？

**口述答案：**

1. `collect_ready_num(timeoutMs)`：调用 `epoll_wait`，返回本轮就绪事件数。如果被信号中断（EINTR），不算错误，返回 0。
2. `collect_active_channels(numReady)`：遍历就绪事件数组，把 `epoll_event.data.ptr`（存的是 Channel 指针）转换回 Channel*，设置 `revents_`，收集到 `activeChannels_`。
3. `resize_event_list(numReady)`：根据负载因子自动扩缩 `eventList_` 缓冲区。就绪事件数超过容量的 90% 就扩容（×1.5），低于 25% 且容量大于初始值就缩容（×0.5）。

这三步保证了 epoll_wait 的结果缓冲区既不会溢出（就绪事件被截断），也不会浪费内存。

---

### Q20：`update_channel` 里的 `assert(findIt->second == channel)` 是在检查什么？

**口述答案：**

这个断言检查的是**同一 fd 是否被不同的 Channel 对象注册**。

`channels_` 是 `unordered_map<int, Channel*>`，key 是 fd。如果同一个 fd 被两个不同的 Channel 注册（比如两个 Channel 都调用了 `enable_reading()`），第二个 Channel 的 `update_in_register` 会走到 `update_channel`，此时 `findIt` 找到了 fd 对应的旧 Channel，但 `findIt->second != channel`（新 Channel），断言失败。

正常情况下这不会发生，因为一个 fd 只对应一个 Channel。这个断言是在防御编程错误——如果有人不小心创建了两个 Channel 绑定同一个 fd，断言会立即暴露问题。

---

### Q21：为什么 `channels_` 用 `unordered_map` 而不是 `map`？

**口述答案：**

`channels_` 的操作只有三种：查找（`find(fd)`）、插入（`channels_[fd] = channel`）、删除（`erase(fd)`）。这三种操作在 `unordered_map` 中都是 O(1) 平均，在 `map` 中是 O(log n)。

`channels_` 不需要有序遍历（不会按 fd 顺序遍历 Channel），所以有序性没有价值。对于 epoll 来说，fd 的数量可能很大（万级甚至十万级），O(1) 和 O(log n) 的差异是可观的。

---

### Q22：`activeChannels_` 为什么是成员变量而不是 `poll()` 的返回值？

**口述答案：**

和 EventLoop 的 `activeChannels_` 一样，是为了避免每轮循环都分配/释放 vector 内存。`activeChannels_` 作为成员变量，`clear()` 后复用底层 buffer。

`poll()` 返回的是 `const vector<Channel*>&`（引用），调用方直接使用这个引用，零拷贝。注意返回的是引用不是值，所以不会有拷贝开销。调用方（EventLoop）在一轮循环内使用这个引用，不会跨轮次持有。

---

### Q23：`epoll_wait` 被信号中断（EINTR）时怎么处理？

**口述答案：**

`collect_ready_num` 中，如果 `epoll_wait` 返回 -1 且 `errno == EINTR`，不算错误，`numReady` 设为 0，函数正常返回。EventLoop 的循环继续——下一轮重新调用 `epoll_wait`。

EINTR 是"系统调用被信号中断"，在网络编程中很常见（比如收到 SIGCHLD、定时器信号等）。它不是致命错误，只需要重试就行。如果用 SA_RESTART 标志注册信号处理函数，大部分系统调用会自动重试，但 epoll_wait 不一定被 SA_RESTART 覆盖，所以手动处理 EINTR 更可靠。

---

## 五、Socket（Q24–Q28）

### Q24：Socket 的移动赋值为什么先关闭自己的旧 fd？

**口述答案：**

```cpp
Socket& Socket::operator=(Socket&& other) {
    if (this != &other) {
        if (sockFd_ >= 0) ::close(sockFd_);  // 关闭自己的旧 fd
        sockFd_ = other.sockFd_;
        other.sockFd_ = -1;
    }
    return *this;
}
```

如果 `this` 当前持有一个有效的 fd，移动赋值意味着"我不再需要这个 fd 了，接管对方的 fd"。如果不先关闭旧 fd，就直接覆盖 `sockFd_`，旧 fd 就泄漏了——没人再持有它，但内核资源没释放。

所以正确顺序是：关闭旧 fd → 接管新 fd → 对方置为无效。

---

### Q25：Socket 的自移动赋值（`sock = std::move(sock)`）安全吗？

**口述答案：**

安全，因为有 `if (this != &other)` 保护。自移动赋值时，`this == &other`，条件不满足，整个操作是 no-op，fd 不变。

C++ 标准要求自移动赋值后对象处于"有效但未指定的状态"。Socket 的实现满足这个要求——自移动后 fd 保持不变（有效状态）。

---

### Q26：Socket 的 fd 为 -1 代表什么？析构时会关闭 -1 的 fd 吗？

**口述答案：**

fd 为 -1 代表"无效/已移动/已关闭"。这是 Unix 的惯例——`open`/`socket`/`accept` 失败时返回 -1。

析构时检查 `if (sockFd_ >= 0)` 才调用 `::close`，所以 -1 的 fd 不会被关闭。这避免了关闭无效 fd 导致的 EBADF 错误。移动构造/赋值后，原 Socket 的 fd 被置为 -1，析构时不会关闭。

---

### Q27：Socket 的 `create_tcp_listener` 为什么要设置 `SO_REUSEADDR`？

**口述答案：**

`SO_REUSEADDR` 允许绑定处于 TIME_WAIT 状态的地址。服务器重启时，之前的连接可能还在 TIME_WAIT（等待 2MSL），如果不设置这个选项，`bind()` 会返回 EADDRINUSE（"Address already in use"），导致服务器无法立即重启。

设置 `SO_REUSEADDR` 后，即使地址处于 TIME_WAIT 状态，`bind()` 也能成功。这对服务器来说几乎是必须的。

---

### Q28：Socket 的 `set_keep_alive` 具体设置了哪些 TCP 参数？

**口述答案：**

除了开启 `SO_KEEPALIVE` 选项外，还设置了三个 TCP 专属参数：

- `TCP_KEEPIDLE = 60`：连接空闲 60 秒后开始发送探测包。
- `TCP_KEEPINTVL = 10`：探测包之间的间隔为 10 秒。
- `TCP_KEEPCNT = 3`：最多发送 3 次探测包。如果 3 次都没收到 ACK，内核认为连接已断开，自动关闭。

这些参数通过 `setsockopt(IPPROTO_TCP, ...)` 设置。`set_keep_alive` 先开启 SO_KEEPALIVE，如果 `on=true` 再设置这三个参数；如果 `on=false`，直接返回（不需要设置参数）。

---

## 六、定时器系统（Q29–Q37）

### Q29：Timer 类保存了哪些状态？它是值对象还是引用对象？

**口述答案：**

Timer 保存四个状态：`id_`（TimerId）、`callback_`（回调函数）、`expiration_`（到期时间，`steady_clock::time_point`）、`interval_`（重复间隔，0 表示一次性）。

Timer 是**值对象**——它只描述"什么时候做什么"，不参与调度编排。调度由 TimerQueue 负责。Timer 不持有 fd、不持有 EventLoop 指针、不操作任何系统资源。

TimerQueue 通过 `shared_ptr<Timer>` 持有 Timer，因为 `timersById_`（主索引）和 `expireHeap_`（懒删除）需要共享访问同一个 Timer 对象。

---

### Q30：TimerId 用 0 表示"无效"，有什么好处？

**口述答案：**

用 0 表示无效是一种**零成本哨兵**设计。`TimerId` 的默认构造函数把 `value_` 设为 0，`valid()` 检查 `value_ != 0`。这意味着：

- 不需要额外的 `bool isValid_` 成员，节省空间。
- 默认构造的 TimerId 天然无效，不需要特殊初始化。
- `TimerQueue` 的 `nextTimerId_` 从 1 开始递增，0 永远不会被分配给真实定时器。

类似的设计在 Unix 中很常见：fd 为 -1 表示无效，指针为 nullptr 表示空。

---

### Q31：`TimerQueue::add_timer` 的 `callback` 参数为什么改成按值传递了？

**口述答案：**

之前是 `const Timer::Callback& callback`（按 const 引用），问题在于：

如果调用方传入一个右值（比如 lambda），按 const 引用接收后，在函数内部 `make_shared<Timer>(id, callback, ...)` 时，`callback` 是一个左值（有名字的引用），只能拷贝给 Timer 的构造函数。即使 Timer 构造函数里做了 `std::move(callback)`，也只是移动了拷贝出来的那份，原始右值的移动语义没用上。

改为按值传递（sink parameter）后：调用方传右值 → 编译器在调用处移动进函数参数 → 函数内部 `std::move(callback)` 移动给 Timer。全程零拷贝，只有两次移动。

---

### Q32：`expireHeap_` 里存的是 `{Timestamp, TimerId}` 对而不是 `shared_ptr<Timer>`，为什么？

**口述答案：**

因为 `expireHeap_` 的唯一用途是**按到期时间排序**，只需要排序键（Timestamp + TimerId 用于区分同一时间戳的定时器）。Timer 对象本身由 `timersById_` 独占持有。

如果 `expireHeap_` 也存 `shared_ptr<Timer>`，每个定时器会多一次 `shared_ptr` 的引用计数操作（构造时 +1，析构时 -1），而且 `priority_queue` 的内部拷贝会触发额外的引用计数原子操作。

只存排序键更轻量：`pair<Timestamp, TimerId>` 是值类型，没有引用计数开销，堆操作更快。查找 Timer 对象时通过 `timersById_.find(timerId)` 按需查找。

---

### Q33：`erase_timer` 为什么只从 `timersById_` 移除，不从 `expireHeap_` 移除？

**口述答案：**

因为 `std::priority_queue` 不支持随机访问和 O(log n) 的指定元素删除。要从堆中间删除一个元素，只能遍历整个堆（O(n)），这对定时器队列来说不可接受。

所以用懒删除策略：`erase_timer` 只从 `timersById_` 移除（O(log n) 的 map 删除），`expireHeap_` 中的残留条目不处理。

等到 `on_timerfd_read()` 收集到期定时器时，从堆顶弹出条目，检查 `timersById_.find(timerId)`——如果找不到，说明已被取消，直接跳过。这样删除操作是 O(log n)，收集时的额外开销只是几次 O(log n) 的 find，均摊下来非常高效。

---

### Q34：`on_timerfd_read` 中的"收集 → 执行 → 同步"管道具体是怎么工作的？

**口述答案：**

**收集阶段**：从 `expireHeap_` 顶部循环弹出。遇到懒删除的条目（`timersById_` 中不存在）就跳过；遇到未到期的（`top.first > now`）就停止；遇到到期的就加入 `expiredTimers` 数组并弹出。

**执行阶段**：遍历 `expiredTimers`，逐个 `timer->run()`。执行后重新检查 `timersById_.find(timer->id())`——因为回调中可能调用了 `erase_timer` 取消了自己或其他定时器。一次性定时器从 `timersById_` 中删除；重复定时器调用 `reschedule(now)` 推进到期时间，重新 `push` 回 `expireHeap_`。

**同步阶段**：调用 `sync_timerfd()`——如果堆非空，用最早到期时间重新武装 timerfd；如果堆空了，解除武装（`it_value = {0,0}`）。

---

### Q35：timerfd 的最小延迟为什么是 1ms？

**口述答案：**

`timerfd_settime` 不允许 `it_value` 为 `{0,0}` 以外的零值。如果计算出的 duration 小于 1ms（比如 0 或负数），`timerfd_settime` 会返回 EINVAL。

`to_timespec` 里的 `if (duration < milliseconds(1)) duration = milliseconds(1)` 是防御性处理：确保传给内核的延迟至少是 1ms，避免系统调用失败。

实际场景中，定时器的精度本身就是毫秒级的（`epoll_wait` 的返回时机受调度延迟影响），1ms 的下限不会影响业务正确性。

---

### Q36：为什么定时器用 `CLOCK_MONOTONIC` 而不是 `CLOCK_REALTIME`？

**口述答案：**

`CLOCK_MONOTONIC` 是单调递增的时钟，不受系统时间调整影响（NTP 校时、管理员手动改时间）。`CLOCK_REALTIME` 是墙钟，可能被 NTP 向前或向后调整。

如果用 `CLOCK_REALTIME`，当系统时间被调快时，定时器可能提前触发；被调慢时，定时器可能延迟触发甚至"消失"（时间回退导致到期时间还没到）。

网络框架的定时器需要稳定可预测，所以统一用 `CLOCK_MONOTONIC`。`steady_clock` 在 Linux 上就是 `CLOCK_MONOTONIC`，和 `timerfd_create(CLOCK_MONOTONIC, ...)` 一致。

---

### Q37：TimerQueue 析构时的顺序为什么重要？

**口述答案：**

TimerQueue 的成员声明顺序是：`timerFd_` → `timerChannel_`。析构时按声明逆序：`timerChannel_` 先析构 → `timerFd_` 后析构。

`timerChannel_` 析构时调用 `disable_all()` + `remove_in_register()`，从 epoll 注销 timerfd。之后 `timerFd_` 析构时调用 `::close(timerfd)`。

如果顺序反过来（fd 先关闭，Channel 后析构），Channel 析构时 `epoll_ctl(DEL)` 操作的是一个已关闭的 fd，会返回 EBADF，触发 `assert(false)`。所以必须保证 Channel 先注销、fd 后关闭。

---

## 七、Buffer（Q38–Q43）

### Q38：Buffer 的三个区域（prependable / readable / writable）是怎么协作的？

**口述答案：**

数据在 Buffer 中的流动方向：

**读方向**（fd → Buffer → 应用层）：`read_from_fd` 把数据写入 writable 区，`writeIndex_` 右移，数据变成 readable。应用层调用 `read_from_buffer` 消费 readable 数据，`readIndex_` 右移，数据变成 prependable。

**写方向**（应用层 → Buffer → fd）：应用层调用 `write_to_buffer` 把数据写入 writable 区。`write_to_fd` 把 readable 数据写入 fd，消费后 `readIndex_` 右移。

**空间复用**：当 readable 数据被全部消费后，`maintain_all_index` 把 `readIndex_` 和 `writeIndex_` 都重置回 `kCheapPrepend` 位置，prependable 和 writable 空间合并复用。这避免了频繁的数据搬移。

---

### Q39：`read_from_fd` 中的栈上额外缓冲区（extraBuf）是做什么的？

**口述答案：**

`read_from_fd` 使用 `readv` 系统调用，同时读到两个目标：Buffer 的 writable 区（iovec[0]）和栈上的 extraBuf（iovec[1]，65536 字节）。

如果对端发来的数据超过了 Buffer 当前的 writable 空间，溢出的数据会留在栈上的 extraBuf 里。`readv` 返回后，再把 extraBuf 中的数据通过 `write_to_buffer` 追加到 Buffer（触发扩容）。

这样设计的好处是：不需要提前知道对端会发多少数据。`readv` 一次系统调用完成"读到 Buffer + 溢出到栈"，比两次 `read` 更高效。栈上 64KB 的额外缓冲是固定的，不涉及堆分配。

---

### Q40：`make_space` 的扩容策略是什么？

**口述答案：**

`make_space` 分两步：

1. **复用 prepend 区**：如果 prependable 空间 + writable 空间足够容纳新数据，就把 readable 数据搬移到 buffer 头部（`std::copy`），prependable 空间并入 writable。读写指针相应调整。
2. **扩容**：如果搬移后还是不够，就调用 `vector::resize` 扩容。

这样设计保证了：只要 buffer 总容量够用，就不需要扩容。只有当总容量确实不够时才触发堆分配。

---

### Q41：Buffer 为什么禁止拷贝但允许移动？

**口述答案：**

拷贝 Buffer 意味着复制整个字节数组（堆分配 + 内存拷贝），对于网络缓冲区来说开销太大且通常没有意义——每个连接有自己独立的 Buffer，不需要共享或复制。

移动 Buffer 只是转移 `vector<char>` 的内部指针（O(1)），在 TcpConnection 的连接表操作中有用（比如从一个 map 移到另一个 map）。

所以 `Buffer(const Buffer&) = delete` 禁止拷贝，`Buffer(Buffer&&) = default` 允许移动。

---

### Q42：`maintain_all_index` 为什么在 readable 为空时重置索引而不是保持原位？

**口述答案：**

如果 readable 为空（数据被全部消费），`readIndex_` 和 `writeIndex_` 可能已经移动到了 buffer 的中间位置。如果不重置，下次写入时 writable 空间只有 readIndex 到 buffer 末尾的部分，prependable 空间白白浪费。

重置后，`readIndex_` 和 `writeIndex_` 都回到 `kCheapPrepend` 位置，整个 buffer 除了头部 8 字节的 prepend 区，其余空间都可以用于写入。这是"读空即复位"策略，最大化空间利用率。

---

### Q43：Buffer 的 `kCheapPrepend` 和 `kInitialSize` 分别是多少？为什么这么选？

**口述答案：**

`kCheapPrepend = 8`，`kInitialSize = 1024`。

8 字节的 prepend 区足够放一个 64 位的长度字段（比如 HTTP 的 Content-Length 或自定义协议的包头）。"cheap" 的意思是"固定开销，不需要搬移数据就能使用"。

1024 字节的初始大小是经验值——对于大多数 HTTP 请求（请求行 + headers），1KB 足够容纳。太小会导致频繁扩容，太大会浪费内存（每个连接都有两个 Buffer）。实际运行中，Buffer 会根据数据量自动扩缩。

---

## 八、Acceptor（Q44–Q47）

### Q44：Acceptor 的 `on_read` 回调做了什么？

**口述答案：**

监听 socket 可读意味着有新连接到达 accept 队列。`on_read` 调用 `Socket::accept`（底层是 `accept4`）接受新连接，返回一个 non-blocking + cloexec 的新 Socket。

然后调用 `handle_connect_callback`，把新 Socket 和对端地址发布给上层（TcpServer 的 `on_connect`）。TcpServer 负责选择 EventLoop、创建 TcpConnection、注册回调。

如果 `accept4` 返回 EMFILE/ENFILE（fd 耗尽），进入 idle fd 恢复流程。

---

### Q45：idle fd 的完整恢复流程是什么？

**口述答案：**

1. `close(idleFd_)`——关闭预置的 pipe 写端，腾出一个 fd 名额。
2. `accept`——拉走 accept 队列中的挂起连接，立即 `close`。这清空了 accept 队列，epoll 不会再重复通知监听 socket 可读。
3. 重建 `idleFd_`——重新 `pipe()` 创建新的 pipe，保留写端作为新的 idle fd。

如果步骤 2 中 `accept` 仍然返回 EMFILE（说明还有更多挂起连接），就循环执行 2-3 直到 accept 返回 EAGAIN。

关键点：步骤 1 关闭 idle fd 后，系统有一个可用的 fd 名额，刚好够 `accept` 用。`accept` 返回的 fd 立即被关闭，不会占用名额。这样避免了 busy-loop——因为 accept 队列被清空了。

---

### Q46：Acceptor 的 `listenSocket_` 声明在 `channel_` 之前，为什么？

**口述答案：**

析构顺序是声明的逆序：`channel_` 先析构 → `listenSocket_` 后析构。

`channel_` 析构时注销 epoll 注册，`listenSocket_` 析构时关闭监听 fd。必须保证 Channel 先注销、fd 后关闭，否则 Channel 析构时 `epoll_ctl(DEL)` 操作的是已关闭的 fd，会触发 assert。

这个模式在项目中是通用的：Socket 声明在 Channel 之前，保证逆序析构时 Channel 先注销、Socket 后关闭。

---

### Q47：Acceptor 的监听 socket 为什么用非阻塞模式？

**口述答案：**

`Socket::create_tcp_listener` 创建 socket 时使用 `SOCK_NONBLOCK | SOCK_CLOEXEC` 标志。

非阻塞的原因：在 Reactor 模型中，所有 fd 都应该是非阻塞的。如果监听 socket 是阻塞的，`accept4` 在没有连接时会阻塞整个 EventLoop 线程，导致其他事件（I/O、定时器）无法处理。

非阻塞模式下，如果没有连接可接受，`accept4` 立即返回 EAGAIN，EventLoop 继续处理其他事件。

`SOCK_CLOEXEC` 保证 fork 出的子进程不会继承这个 fd，避免资源泄漏。

---

## 九、TcpConnection（Q48–Q55）

### Q48：TcpConnection 为什么必须用工厂方法而不是直接构造？

**口述答案：**

因为 TcpConnection 继承了 `enable_shared_from_this`。`shared_from_this()` 只有在对象已经被 `shared_ptr` 管理时才能调用。

如果用 `new TcpConnection(...)` 直接构造，在构造函数内部调用 `shared_from_this()` 会崩溃——因为此时还没有 `shared_ptr` 指向它。

工厂方法 `create_connection` 解决了这个问题：
1. `new TcpConnection(...)` 构造对象。
2. 用 `shared_ptr` 包装。
3. 调用 `channel_->tie_to_object(conn->shared_from_this())`。
4. 调用 `channel_->enable_reading()`。

这样 `shared_from_this()` 在步骤 3 调用时，`shared_ptr` 已经存在了（步骤 2 创建的）。

---

### Q49：TcpConnection 的 `send` 方法有两个重载（`const string&` 和 `string&&`），为什么？

**口述答案：**

`send(const string& msg)` 是左值引用版本：调用方传入的 string 不会被修改，`send_in_loop` 内部会拷贝到 writeBuffer。

`send(string&& msg)` 是右值引用版本：调用方用 `std::move` 传入，`send_in_loop` 内部可以 `std::move` 进 writeBuffer，省去一次堆分配 + 内存拷贝。

在高频写入场景（比如广播消息给大量连接），这个优化很关键——每条消息省一次拷贝，百万连接就是百万次拷贝。

---

### Q50：TcpConnection 的 `send` 为什么检查 `is_in_loop_thread()`？

**口述答案：**

因为 `send` 可能从任意线程调用（比如用户在主线程发送广播），但 Buffer 和 Channel 的操作必须在所属 EventLoop 线程执行。

如果当前是所属线程，直接调用 `send_in_loop`（同步执行，零延迟）。如果不是，通过 `run_in_loop` 投递到所属线程（异步执行，有一次 eventfd 唤醒的开销）。

这保证了线程安全——`send_in_loop` 中对 `writeBuffer_` 和 `channel_` 的操作都在单线程中执行，不需要加锁。

---

### Q51：TcpConnection 的 `on_read` 中 read 返回 0 意味着什么？

**口述答案：**

read 返回 0 表示对端优雅关闭了连接（发送了 FIN）。这是 TCP 四次挥手的正常流程。

此时 `on_read` 调用 `close_connection`，触发关闭流程：
1. 设置 `isClosed_ = true`（保证幂等）。
2. `disable_reading()` + `disable_writing()`——停止关注所有事件。
3. 触发 `closeCallback_`——通知 TcpServer 从连接表中移除这个连接。

注意：read 返回 0 不是错误，是正常的连接关闭信号。read 返回 -1 且 errno 不是 EAGAIN/EWOULDBLOCK 才是真正的错误。

---

### Q52：TcpConnection 的 `isClosed_` 标志是做什么的？

**口述答案：**

`isClosed_` 保证 `close_connection` 的**幂等性**——多次调用不会重复执行关闭流程。

可能触发关闭的场景有多个：read 返回 0（EOF）、read/write 返回错误、EPOLLHUP 事件、EPOLLERR 事件、`force_close()` 主动关闭。这些场景可能在同一次事件循环中同时发生（比如 EPOLLHUP + EPOLLIN 同时到来）。

如果没有 `isClosed_` 保护，`close_connection` 可能被执行多次，导致重复触发 `closeCallback_`（TcpServer 重复从连接表移除），甚至重复操作已关闭的 fd。`isClosed_` 确保只有第一次调用会执行实际的关闭逻辑。

---

### Q53：TcpConnection 的 `force_close` 和被动关闭有什么区别？

**口述答案：**

**被动关闭**：由事件驱动——read 返回 0（EOF）或错误，`on_read` → `close_connection`。这是正常的连接关闭流程。

**主动关闭**（`force_close`）：由上层主动调用——比如心跳检测发现连接超时、业务逻辑决定断开连接。`force_close` 内部调用 `force_close_in_loop`，最终走和被动关闭相同的 `close_connection` 路径。

区别在于触发方式不同，但最终的关闭路径是共享的（`close_connection`）。`force_close` 也需要检查线程——如果不是所属线程，通过 `run_in_loop` 投递。

---

### Q54：TcpConnection 的高水位回调机制是怎么工作的？

**口述答案：**

当 `send_in_loop` 往 `writeBuffer_` 追加数据后，检查 `writeBuffer_->readable_bytes()` 是否超过 `highWaterMark_`（默认 64MB）。如果超过，触发 `highWaterMarkCallback_`。

高水位回调的用途是**背压控制**：当发送速度远大于接收速度时，`writeBuffer_` 会不断增长。如果不做限制，可能耗尽内存。高水位回调通知上层"发送缓冲积压太多"，上层可以采取措施——比如暂停读取（`disable_reading()`）、限流、或者关闭连接。

这是 TCP 流量控制在应用层的补充——TCP 的窗口机制控制的是内核发送缓冲区，高水位控制的是应用层发送缓冲区。

---

### Q55：TcpConnection 的 `channel_` 声明在 `connSocket_` 之后，为什么？

**口述答案：**

析构顺序是声明的逆序：`channel_` 先析构 → `connSocket_` 后析构。

`channel_` 析构时：`disable_all()` + `remove_in_register()`，从 epoll 注销 fd。
`connSocket_` 析构时：`::close(fd)`，关闭连接 fd。

必须保证 Channel 先注销 epoll、Socket 后关闭 fd。如果反过来，Channel 析构时 `epoll_ctl(DEL)` 操作的是已关闭的 fd，会触发 assert。

---

## 十、TcpServer（Q56–Q61）

### Q56：TcpServer 的 `start()` 做了什么？

**口述答案：**

`start()` 按顺序做四件事：

1. 创建 `EventLoopThreadPool` 并 `start()`，启动所有 IO 线程。
2. 创建 `Acceptor`，绑定监听地址，注册 `on_connect` 回调。
3. 设置 `state_ = Running`。
4. 进入主 EventLoop 的 `loop()`——主线程在这里循环，处理 accept 事件。

新连接到达时，`on_connect` 被调用，选择一个 IO EventLoop，创建 TcpConnection，注册回调，放入连接表。

---

### Q57：TcpServer 的连接分配策略是什么？

**口述答案：**

Round-Robin（轮询）。`EventLoopThreadPool::get_next_loop()` 用 `atomic<size_t>` 的 `ioLoopsIndex_` 做无锁轮询，每次返回下一个 IO EventLoop。

如果 IO 线程数为 0（单线程模式），`get_next_loop()` 回退到主 EventLoop，所有连接都在主线程处理。

Round-Robin 的优点是简单、无锁、均匀分配。缺点是不感知各 EventLoop 的实际负载——如果某些连接特别繁忙（比如大文件传输），可能会导致某个 EventLoop 过载。

---

### Q58：TcpServer 的 `connectionRecordsByLoop_` 为什么是两层哈希？

**口述答案：**

外层 `unordered_map<EventLoop*, ConnectionRecords>` 按 EventLoop 分片，内层 `ConnectionRecords`（`unordered_map<TcpConnection*, ConnectionRecord>`）按连接指针索引。

外层在 `start()` 阶段一次性初始化完毕（每个 EventLoop 一个空 map），运行期只读不写。多线程并发查找（`find(ioLoop)`）天然安全（读-读不竞争）。

内层严格遵守 Thread-Per-Core 原则：只有该 EventLoop 所属线程才能增删改查。因为一个连接的所有操作都在同一个 EventLoop 线程执行，所以内层也不需要锁。

这样整个连接管理全程无锁。

---

### Q59：TcpServer 的 `stop()` 和 `shutdown_connections` 是怎么协作的？

**口述答案：**

`stop()` 设置 `state_ = Draining`，然后调用主 EventLoop 的 `quit()`。主 EventLoop 退出 `loop()` 后，调用 `shutdown_connections()`。

`shutdown_connections` 遍历所有连接，逐个 `force_close()`。然后等待 `activeConnectionCount_` 归零（通过 `shutdownCondition_` 条件变量等待）——每个连接关闭后会触发 `on_close`，`on_close` 中 `activeConnectionCount_` 递减，归零时唤醒主线程。

这保证了 `start()` 返回时，所有连接已经优雅关闭，所有资源已经释放。

---

### Q60：TcpServer 的 `on_close` 回调做了什么？

**口述答案：**

`on_close` 在连接所属的 EventLoop 线程中执行：

1. 调用 `remove_connection`——从该 EventLoop 的 `connectionRecordsByLoop_` 中移除连接记录（包括心跳对象）。
2. 通知用户的 `closeCallback_`。
3. `activeConnectionCount_` 递减，如果归零则 `shutdownCondition_` 通知。

注意：`on_close` 不直接销毁 TcpConnection——连接的 `shared_ptr` 从连接表中移除后，如果没有其他持有者，引用计数归零，自动析构。

---

### Q61：TcpServer 的心跳检测是怎么集成的？

**口述答案：**

用户通过 `set_connection_heartbeat(interval, timeout)` 配置全局心跳参数。这些参数保存在 `ConnectionHeartbeatOptions` 中，不立即生效。

每当新连接建立（`create_connection`），检查 `connectionHeartbeatOptions_.enabled`，如果启用了，就创建一个 `ConnectionHeartbeat` 对象，绑定到连接上，调用 `start()` 启动周期定时器。

每收到消息（`on_message`），调用 `refresh_connection_heartbeat` 刷新最后活跃时间。

连接关闭时（`remove_connection`），心跳对象随连接记录一起销毁（`shared_ptr` 引用计数归零），定时器自动取消。

---

## 十一、EventLoopThread 与线程池（Q62–Q65）

### Q62：EventLoopThread 的构造函数为什么用条件变量等待？

**口述答案：**

构造函数启动后台线程后，必须等待线程内的 EventLoop 创建完毕才能返回。否则调用方立即调用 `get_loop()` 可能得到 nullptr。

`thread_func` 中，创建 EventLoop 后用 `loopCondition_.notify_one()` 通知。构造函数用 `loopCondition_.wait(lock, [this]() { return loop_ != nullptr; })` 等待，直到 `loop_` 非空才返回。

这保证了构造函数返回时，后台线程的 EventLoop 已经就绪，`get_loop()` 一定返回有效指针。

---

### Q63：EventLoopThread 的析构函数为什么要 `quit()` 再 `join()`？

**口述答案：**

`quit()` 通知后台线程的 EventLoop 退出循环。`join()` 等待后台线程完整执行完 `thread_func`（包括 EventLoop 析构）后返回。

如果不 `quit()`，后台线程的 EventLoop 可能永远阻塞在 `epoll_wait` 中，`join()` 永远不返回（死锁）。

如果不 `join()`，`EventLoopThread` 析构时 `std::thread` 对象处于 joinable 状态，`std::thread` 的析构函数会调用 `std::terminate()`，直接终止进程。

所以正确顺序是：先 `quit()` 让 EventLoop 退出，再 `join()` 等线程结束。

---

### Q64：EventLoopThreadPool 的 `mainLoop_` 为什么是 `unique_ptr<EventLoop>` 而不是 `unique_ptr<EventLoopThread>`？

**口述答案：**

因为 main loop 运行在调用 `start()` 的**当前线程**（通常是主线程），不是在一个新的后台线程中。`EventLoopThread` 的职责是"创建新线程并在其中运行 EventLoop"，对 main loop 来说不需要创建新线程。

所以 `mainLoop_` 直接持有 `EventLoop` 对象，在 `start()` 中通过 `create_main_loop()` 在当前线程构造。IO 线程才需要 `EventLoopThread`（`ioLoopThreads_`）。

---

### Q65：`get_next_loop()` 在 IO 线程数为 0 时回退到 main loop，这意味着什么？

**口述答案：**

这意味着**单线程模式**——没有 IO 线程，所有连接（包括 accept）都在主线程处理。main loop 既负责 accept 新连接，又负责所有连接的 I/O。

这对小规模服务（几百个连接）是够用的，避免了线程切换开销。但对大规模服务，主线程会成为瓶颈——accept 和 I/O 在同一个线程中，高连接率时可能来不及处理 I/O。

`ioLoopsIndex_` 的 `fetch_add` 在空池时不会溢出（对 0 取模回退），所以逻辑是安全的。

---

## 十二、HTTP 层（Q66–Q72）

### Q66：HttpContext 为什么禁止拷贝和移动？

**口述答案：**

因为 `llhttp` 内部保存了指向 `HttpContext` 的裸指针（通过 `parser_.data = this`）。llhttp 的静态回调（`on_url`、`on_header_field` 等）通过 `parser->data` 恢复 `HttpContext` 指针，调用对应的 `*_impl` 方法。

如果 HttpContext 被拷贝或移动，`this` 指针变了，但 `parser_.data` 还指向旧地址，后续回调会访问已释放的内存（use-after-free）。

所以 HttpContext 禁止拷贝和移动，保证 `this` 指针在生命周期内不变。

---

### Q67：HttpContext 的"静态桥"回调模式是什么？

**口述答案：**

llhttp 的回调必须是 `static` 函数（C 语言风格的函数指针），不能是成员函数。但 `static` 函数没有 `this` 指针，无法访问 HttpContext 的实例成员。

解决方案是"静态桥"：

1. `llhttp_settings_t` 中注册静态回调（如 `on_url`）。
2. `parser_.data = this` 把 HttpContext 指针存入 parser 的 user data。
3. 静态回调内部通过 `get_context(parser)` 恢复 HttpContext 指针：`return static_cast<HttpContext*>(parser->data)`。
4. 调用实例方法（如 `on_url_impl(at, length)`）。

这是 C 回调 + C++ 对象的经典桥接模式。

---

### Q68：Router 的 dispatch 流程是什么？

**口述答案：**

固定四步流水线：

1. **精确匹配**：用 `method + path` 查 `exactRoutes_`（`unordered_map`），O(1) 平均。
2. **405 判定**：精确匹配失败时，查 `allowedMethodsByPath_` 看该路径是否有其他方法注册了。如果有，返回 405 Method Not Allowed + Allow 头。
3. **前缀兜底**：按注册顺序遍历 `prefixRoutes_`，找第一个前缀匹配的处理器。
4. **404 回退**：前缀也没匹配到，返回 404 Not Found。用户可以通过 `set_not_found_handler` 自定义 404 响应。

405 判定在 404 之前，这是 HTTP 规范的要求——如果路径存在但方法不对，应该返回 405 而不是 404。

---

### Q69：Router 的精确路由键 `RouteKey` 的哈希策略是什么？

**口述答案：**

`RouteKey` 由 `method` 和 `path` 两个 string 组成。`RouteKeyHash` 把两个 string 的 `std::hash` 值做异或（XOR）组合。

这是一个简单但有效的哈希策略——两个不同的 RouteKey（method 不同或 path 不同）大概率产生不同的哈希值。极端情况下（method 和 path 的哈希值恰好相同），XOR 会碰撞，但 `unordered_map` 内部有链式冲突处理，不会影响正确性。

---

### Q70：HttpResponse 的 `package_to_string` 序列化了哪些部分？

**口述答案：**

按 HTTP 报文格式，依次序列化三部分：

1. **状态行**：`HTTP/1.1 200 OK\r\n`
2. **响应头**：逐个追加 `field: value\r\n`。如果 `closeConnection_` 为 true，自动补 `Connection: close` 头。
3. **空行 + 响应体**：`\r\n` 分隔头部和 body，然后追加 body 内容。

序列化结果是一个完整的 HTTP 响应报文，可以直接通过 `TcpConnection::send` 发送。

---

### Q71：HttpServer 的连接级状态（ConnectionState）包含什么？为什么需要它？

**口述答案：**

每个连接的 `ConnectionState` 包含：
- `HttpContext httpContext`：该连接的 HTTP 解析状态机（llhttp 实例 + 请求构建中间状态）。
- `unique_ptr<TlsConnection> tlsConnection`：该连接的 TLS 状态（如果启用了 HTTPS）。

为什么需要连接级状态？因为 HTTP 是基于 TCP 连接的协议，一个 TCP 连接可能承载多个 HTTP 请求（Keep-Alive）。每个连接需要独立的解析状态——llhttp 的状态机是逐连接的，不能多个连接共享。

ConnectionState 通过 `unordered_map<TcpConnection*, shared_ptr<ConnectionState>>` 管理，连接建立时创建，连接关闭时移除。

---

### Q72：HttpServer 处理 HTTPS 和 HTTP 的区别在哪？

**口述答案：**

在 `send_response` 中，如果 `sslContext_` 存在且连接有 `tlsConnection`，数据先通过 `TlsConnection::encrypt` 加密，再通过 `TcpConnection::send` 发送密文。接收时，数据先通过 `TlsConnection::decrypt` 解密，再交给 `HttpContext::parse` 解析 HTTP。

HTTP 路径直接收发明文，不经过 TLS 层。两种路径共享同一个 `Router` 和业务处理器，对用户透明。

`enable_ssl` 必须在 `start()` 前调用，因为 TLS 上下文（证书、私钥）需要在 accept 新连接前初始化完毕。

---

## 十三、SSL/TLS（Q73–Q74）

### Q73：SslContext 和 TlsConnection 的职责分别是什么？

**口述答案：**

`SslContext` 是**全局 TLS 上下文**：持有 SSL_CTX（证书、私钥、协议配置），在 HttpServer 生命周期内只有一个实例。`enable_ssl` 时初始化，所有连接共享。

`TlsConnection` 是**单连接 TLS 状态**：持有 SSL 对象（一个 TLS 会话的状态机），每个 HTTPS 连接一个实例。负责该连接的 TLS 握手、加密、解密。

关系：`SslContext` 创建 `SSL*` 对象，注入到 `TlsConnection` 中。`TlsConnection` 通过 `SSL*` 做加解密操作。

---

### Q74：TlsConnection 的 `handshake` 是在什么时候执行的？

**口述答案：**

TlsConnection 的 handshake 不是显式调用的——它在 `decrypt` 过程中自然发生。

TLS 握手协议的数据是通过 TCP 连接传输的。客户端发送 ClientHello 时，数据通过 `TcpConnection::on_read` → `HttpServer::on_message` 到达 `TlsConnection::decrypt`。`decrypt` 内部调用 `SSL_read`，OpenSSL 发现这是握手数据，自动完成握手流程。

服务端的 ServerHello、Certificate 等响应也是通过 `SSL_read`/`SSL_write` 自动处理的。整个握手过程对上层透明。

---

## 十四、ConnectionHeartbeat（Q75–Q77）

### Q75：ConnectionHeartbeat 的 `weak_ptr<TcpConnection>` 有什么好处？

**口述答案：**

用 `weak_ptr` 而不是 `shared_ptr` 持有连接，避免了**循环引用**。

如果心跳对象用 `shared_ptr` 持有连接，而连接的 `ConnectionRecord` 又用 `shared_ptr` 持有心跳对象，两者互相引用，引用计数永远不归零，内存泄漏。

用 `weak_ptr` 后，心跳对象不增加连接的引用计数。连接关闭时，`ConnectionRecord` 被移除，连接的 `shared_ptr` 引用计数归零，连接析构。心跳对象的定时器回调中 `conn_.lock()` 返回 nullptr，自动停止检测。

---

### Q76：心跳检测的 `refresh` 和 `check_timeout` 是怎么配合的？

**口述答案：**

`refresh()` 更新 `lastActiveTime_` 为当前时间。每次收到消息时由 TcpServer 调用。

`check_timeout()` 是周期定时器的回调，计算 `now - lastActiveTime_`，如果超过 `idleTimeoutSeconds_`，调用 `conn->force_close()` 关闭连接。

两者的配合：只要连接活跃（有消息收发），`refresh` 不断更新 `lastActiveTime_`，`check_timeout` 永远不会超时。如果连接长时间无消息，`lastActiveTime_` 停止更新，`check_timeout` 检测到超时后关闭连接。

---

### Q77：心跳检测的 `start/stop/refresh` 为什么都有 `*_in_loop` 版本？

**口述答案：**

因为 `start/stop/refresh` 可能从任意线程调用（比如在主线程配置心跳参数，在 IO 线程刷新活跃时间），但定时器操作和状态修改必须在连接所属的 EventLoop 线程执行。

`start_in_loop` 在 EventLoop 线程中创建周期定时器。`stop_in_loop` 取消定时器。`refresh_in_loop` 更新 `lastActiveTime_`。

非 `_in_loop` 版本检查线程——如果是所属线程直接调用，否则通过 `run_in_loop` 投递。

---

## 十五、InetAddress（Q78）

### Q78：InetAddress 构造时为什么要校验 `AF_INET`？

**口述答案：**

`InetAddress(sockaddr_in)` 构造函数调用 `ensure_ipv4_family(address)` 检查 `address.sin_family == AF_INET`。

因为 `sockaddr_in` 是 IPv4 专用的结构体，如果传入 IPv6 的 `sockaddr_in6`（强制转换为 `sockaddr_in`），数据布局完全不对（IPv6 地址 128 位，IPv4 只有 32 位），后续 `bind/connect` 会传入错误的地址，导致不可预测的行为。

断言校验在 debug 模式下立即暴露错误，release 模式下零开销（assert 被编译器移除）。

---

## 十六、测试与工程质量（Q79–Q80）

### Q79：单元测试中 Channel 的生命周期问题是怎么处理的？

**口述答案：**

Channel 必须在 EventLoop 线程内析构（析构函数有 `assert_in_loop_thread()`），但测试函数运行在主线程。如果 Channel 在测试函数结束时被 `shared_ptr` 析构，就是在主线程析构，触发 assert。

解决方案：所有 Channel 测试用 `shared_ptr<Channel>` 在测试作用域持有 Channel。需要销毁时，通过 `run_in_loop([&]() { ch.reset(); })` 在 EventLoop 线程内安全销毁。或者在定时器回调中销毁（定时器回调在 EventLoop 线程内执行）。

另外，socketpair fd 注册了 EPOLLIN 时，read 回调中需要 `::read()` 消费数据，否则 LT 模式下 epoll 会无限触发回调。

---

### Q80：项目的单元测试覆盖了哪些类？有哪些边界条件被测试？

**口述答案：**

覆盖了所有核心类：TimerId、Timer、TimerQueue、Socket、Channel、EpollPoller、EventLoop、EventLoopThread、EventLoopThreadPool、Buffer、Acceptor、TcpConnection、TcpServer、ConnectionHeartbeat、HttpRequest、HttpResponse、HttpContext、Router、HttpServer、SslContext、TlsConnection、InetAddress、NonCopyable。

关键边界条件：
- **Socket**：无效 fd（-1）、自移动赋值、移动赋值关闭旧 fd、析构关闭 fd 验证。
- **Timer**：空回调不崩溃、周期定时器 reschedule 推进时间、TimerId 排序和相等。
- **TimerQueue**：取消不存在的定时器、重复取消、回调中取消自身、回调中添加新定时器、跨线程取消。
- **Channel**：默认无事件、enable/disable 读写组合、未设置写回调不崩溃、tie 过期阻止回调。
- **Buffer**：readv 栈溢出缓冲、prepend 区复用、读空后索引重置。
- **EventLoop**：跨线程投递、run_after 定时器精度、run_every 取消。
- **Router**：精确匹配优先、405 区分、前缀兜底、自定义 404/405 处理器。

---

*以上 80 问覆盖了 Tudou 框架的每一个类、每一个设计决策、每一个实现细节，适合作为深度技术面试的全面准备。*