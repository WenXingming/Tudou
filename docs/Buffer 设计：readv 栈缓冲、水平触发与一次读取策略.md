# Buffer 设计：readv 栈缓冲、水平触发与一次读取策略

本文结合陈硕对 muduo Buffer 的说明，以及 Tudou 当前源码中的 `Buffer`、`TcpConnection`、`Channel`、`EpollPoller`、`Socket` 实现，采用 STAR 法则把这几个问题讲清楚：

1. 为什么非阻塞网络库还需要应用层 Buffer
2. 为什么 Buffer 不能简单地“一上来就开很大”
3. `readv + 栈上 extrabuf` 到底巧妙在哪里
4. 为什么当前实现更适合使用 LT（水平触发），而不是 ET（边缘触发）
5. 面试时应该怎么把这套设计讲明白

这篇文档的重点不是背结论，而是建立一条完整因果链：

> 非阻塞 Reactor 需要应用层 Buffer 来承接内核 socket 缓冲区与上层协议之间的状态；
> 为了同时兼顾内存占用和系统调用效率，输入缓冲区常驻部分做小，突发流量靠一次 `readv` 临时借用栈空间吸收；
> 而“每次可读只读一次”的策略之所以成立，是因为当前事件通知语义是 LT，不是 ET。

---

## 1. Situation（情境）

### 1.1 非阻塞网络编程里，Buffer 解决的到底是什么问题？

很多人一开始会问：

> 内核不是已经有 socket 接收缓冲区和发送缓冲区了吗，为什么应用层还要再搞一层 Buffer？

原因是：**内核缓冲区只负责内核态的数据暂存，不负责应用层的协议边界、半包/粘包、短写重试、消息组装和用户态状态管理。**

以 Tudou 当前实现为例：

- `TcpConnection` 内部有两个 `Buffer`：`readBuffer_` 和 `writeBuffer_`
- `readBuffer_` 用来承接从 fd 读出来、但上层还没消费完的数据
- `writeBuffer_` 用来承接上层想发、但内核这次还没完全写走的数据

也就是说，Buffer 的职责不是“替代内核缓冲区”，而是：

1. 在用户态保存连接的收发进度
2. 处理非阻塞 IO 下常见的“这次没读完 / 这次没写完”
3. 把“字节搬运”从业务逻辑里剥离出去

所以，**Buffer 是连接状态的一部分，不是一个可有可无的小工具。**

### 1.2 真正的矛盾：吞吐效率和内存占用是对着干的

输入缓冲区设计时有一对天然冲突：

- 缓冲区太小：一次可读事件可能需要多次 `read` / `readv`，系统调用开销增加
- 缓冲区太大：大部分连接长期闲置，却要常驻占很多内存

这就是陈硕在 muduo 里强调的那个问题。

如果有 10000 个连接，而每个连接一建立就分配几十 KB 的读缓冲和写缓冲，那么大多数连接明明没多少流量，却会白白占掉数百 MB 甚至 GB 级内存。

Tudou 当前实现显然不想这么做：

- `Buffer::kInitialSize = 1024`
- `Buffer::kCheapPrepend = 8`

也就是说，**每个 Buffer 常驻的初始字节区只有 1 KB 级别，而不是一上来就分配 64 KB。**

### 1.3 当前仓库里的前提：这是 LT + 非阻塞 socket 的组合

这一点必须先说清楚，否则后面对“为什么只读一次”的理解会跑偏。

Tudou 当前代码里：

- 监听 socket 用 `socket(..., SOCK_NONBLOCK | SOCK_CLOEXEC, ...)` 创建
- 已连接 socket 用 `accept4(..., SOCK_NONBLOCK | SOCK_CLOEXEC)` 接收
- `Channel` 注册的事件只有 `EPOLLIN | EPOLLPRI` 和 `EPOLLOUT`
- `EpollPoller` 只是把这些事件原样传给 `epoll_ctl`
- 代码里没有给连接 fd 打开 `EPOLLET`

这意味着：

1. 当前连接 fd 是**非阻塞**的
2. 当前 epoll 触发语义是**默认 LT（水平触发）**

这两个前提一起决定了：

- `read` / `readv` 现在做不了时会返回 `EAGAIN` / `EWOULDBLOCK`，不会把事件循环卡死
- 就算某次没把内核接收缓冲区读空，只要 fd 仍然可读，后续 `epoll_wait` 还会继续返回它

后面所有设计判断，都是建立在这两个事实之上的。

---

## 2. Task（任务）

在这样的网络模型里，Buffer 需要同时满足五个目标：

### 2.1 常驻内存要小

不能因为“也许将来会来一大包数据”，就给每条连接都常驻分配一个大缓冲区。

### 2.2 可读事件里尽量少做系统调用

如果明明一次就能把当前内核缓冲区里的大部分数据取走，却拆成两三次 `read`，那就是白白增加用户态/内核态切换成本。

### 2.3 正确处理非阻塞 IO 的短读、短写和 EAGAIN

Buffer 必须把这些都变成正常控制流，而不是异常情况。

### 2.4 不让单个热点连接长期霸占事件循环

高并发 Reactor 的核心不是“把一个连接吃干榨净”，而是“让很多连接都能及时被轮到”。

### 2.5 职责只停留在字节搬运，不掺业务语义

Buffer 不负责 HTTP、业务协议、路由或应用逻辑。它只负责：

- 字节怎么放进来
- 字节怎么拿出去
- 空间怎么复用
- fd 怎么衔接

这也是当前 `Buffer.h` 文件头注释强调的边界：**它只是 TCP 子系统的纯工具层。**

---

## 3. Action（行动）

## 3.1 先看数据结构：一个连续数组，三个逻辑区间

Tudou 的 `Buffer` 使用一个 `std::vector<char>` 作为底层连续内存，并用两个索引维护三个逻辑区间：

```text
+-------------------+------------------+------------------+
| prependable bytes |  readable bytes  |  writable bytes  |
+-------------------+------------------+------------------+
0               readIndex_         writeIndex_        buffer_.size()
```

含义分别是：

- `prependable bytes`：头部可复用区域
- `readable bytes`：当前已经有数据、可供上层读取的区域
- `writable bytes`：尾部尚未使用、可继续写入的区域

当前实现中的两个关键索引是：

- `readIndex_`：可读区起点
- `writeIndex_`：可读区终点，也是可写区起点

因此：

- `readable_bytes() = writeIndex_ - readIndex_`
- `writable_bytes() = buffer_.size() - writeIndex_`

这个模型的优点是：

1. 顺序写入很自然，只需要推进 `writeIndex_`
2. 顺序读取很自然，只需要推进 `readIndex_`
3. 读空后可以整体复位，不必频繁释放/重新申请内存

### 3.1.1 为什么要保留 prepend 区？其核心设计思想是什么？

这里的 `kCheapPrepend = 8` 字节预留空间是网络库中为了实现**“零拷贝包头追加”**而设计的经典机制。虽然当前 Tudou 框架的 HTTP 业务层在发送消息时直接顺序写入包头与包体，没有显式暴露并使用 `prepend()` 系列接口，但这块区域在架构设计上具有极其重要的潜在作用。如果上层在消息组装完成的一刻，业务层算出了包体的总长度（例如 100 字节），并需要在这个消息的前面加上一个 4 字节的长度信息 作为包头，此时，`kCheapPrepend` 的存在就可以让我们在不额外开辟大内存的情况下，直接在 Buffer 的头部预留区写入包头，从而实现零拷贝的包头拼接（无需额外分配内存，也无需搬移包体数据）。

#### 1. 核心设计思想：自定义协议的零拷贝包头拼接 (Zero-Copy Header Prepends)

在许多自定义二进制 RPC 协议（如 Dubbo、gRPC 等）中，为了解决 TCP 粘包问题，发送报文通常采用如下结构：
* **4 字节魔数 (Magic Number)**：用于标识协议身份。
* **4 字节包长 (Length)**：代表后续序列化数据的字节大小。
* **N 字节包体 (Payload)**：如序列化后的 Protobuf 二进制数据。

如果缓冲区没有头部预留区（即读指针初始在 `0` 字节），当我们序列化数据时：
1. **方案 A (慢)**：必须先开辟一块 `N+8` 字节的临时大内存，拷贝 8 字节包头，再拷贝 `N` 字节包体，最后送去发送。这引入了**额外的动态内存分配和两次内存拷贝**。
2. **方案 B (慢)**：直接在原 Buffer 写入 `N` 字节包体，然后使用 `memmove` 将这 `N` 字节数据在内存中整体往右平移 8 字节，腾出最前方的 `[0-7]` 字节写包头。这引入了 **$O(N)$ 的内存搬移开销**。

#### 2. 利用 `kCheapPrepend` 实现零拷贝的实现与使用示例

有了头部预留区，我们可以在 `Buffer` 中轻松扩展一个 `prepend()` 方法，并实现零拷贝拼接：

```cpp
// 1. Buffer 底层的 prepend 写入实现
void Buffer::prepend(const void* data, size_t len) {
    assert(len <= prependable_bytes()); // 确保头部空洞容量足够
    readIndex_ -= len;                 // 读指针前移，空出写入区间
    const char* src = static_cast<const char*>(data);
    std::copy(src, src + len, buffer_.begin() + readIndex_);
}
```

在业务层发送 Protobuf 消息时的具体执行流程与内存演进如下：

* **第一步：Buffer 初始化**
  读写指针均位于 `kCheapPrepend` (8 字节) 偏移处，头部预留出 8 字节空洞。
  ```text
  [ 8 字节头部预留区 ] [    1024 字节空闲可写区 (InitialSize)    ]
  ^                  ^
  0                  readIndex_ / writeIndex_ (指向偏移 8)
  ```

* **第二步：直接将包体序列化至 Buffer 尾部**
  假设 Protobuf 序列化后的数据长 100 字节，业务层直接调用 `write_to_buffer()` 顺序写入：
  ```cpp
  std::string protobuf_data = "...(100字节)...";
  buf.write_to_buffer(protobuf_data);
  ```
  此时包体落入可读区，写指针向后移动 100 字节，**头部 8 字节依然处于空闲状态**：
  ```text
  [ 8 字节头部预留区 ] [ 100 字节包体数据 ] [    924 字节空闲可写区    ]
  ^                  ^                  ^
  0                  readIndex_(指向8)  writeIndex_(指向108)
  ```

* **第三步：零拷贝强塞包头**
  包体写完后，业务层计算出包体长度为 100，随后生成魔数与长度，并向前反向写入预留区：
  ```cpp
  uint32_t magic = htonl(0x5444555F); // 4 字节魔数 "TDU_"
  uint32_t length = htonl(100);        // 4 字节包体长度
  
  buf.prepend(&length, sizeof(length)); // 长度信息写入 [4~7] 字节，readIndex_ 变 4
  buf.prepend(&magic, sizeof(magic));   // 魔数信息写入 [0~3] 字节，readIndex_ 变 0
  ```
  此时头部预留区被完美填满，读指针前移至 0。
  ```text
  [ 4字节魔数 ] [ 4字节包长 ] [ 100 字节包体数据 ] [    924 字节空闲可写区    ]
  ^                                            ^
  readIndex_(指向0)                            writeIndex_(指向108)
  ```

* **第四步：物理发送**
  调用 `write_to_fd()`，一次系统调用将连续内存中的 108 字节全部送出，**全程没有发生任何包体数据的内存搬移或二次拷贝**。

#### 3. 现在的现实作用

即便当前仓库的业务没有直接调用 `prepend()`，这块预留区也发挥着两个物理层面的基准作用：
1. **索引复位基准**：在 `maintain_all_index()` 时作为对齐锚点，确保即使在缓冲区读空复位后，其相对偏置也是固定且对齐的。
2. **整理数据的物理隔离**：在 `make_space()` 对数据进行前移回收时，将数据搬移至 `kCheapPrepend` 偏置之后，在物理上防止由于没有头部间隔导致的零碎越界。

### 3.1.2 空间不够时怎么处理？

`write_to_buffer()` 不会盲目扩容，而是先调用 `make_space(len)`：

- 如果尾部可写空间不够，但“尾部可写 + 头部可复用”总和足够，就用 `memmove` 把可读数据搬回前面，复用空洞
- 只有复用后仍然不够，才真正 `resize`

这一步很关键，因为它避免了“明明前面有很多已经读走留下来的空位，却还在不断扩容”的低效行为。

另外，源码这里使用的是 `memmove` 而不是 `std::copy`，原因也很重要：

> 可读区和目标区可能重叠，重叠搬移必须使用 `memmove`，否则行为未定义。

这类细节在面试里很容易加分，因为它说明你不是只背了个大框架，而是真的看过实现。

---

## 3.2 读路径的核心：小常驻 Buffer + 一次 `readv` + 栈上 `extrabuf`

这正是整套设计最精巧的部分。

### 3.2.1 先说直觉

我们希望：

- 平时每个连接只常驻很小的 Buffer，省内存
- 一旦这次来了较多数据，又尽量一次系统调用就把它们拿回来，省系统调用

如果只选前者，缓冲区太小，就会多次 `read`

如果只选后者，缓冲区太大，就会让每个连接长期背着一大块闲置内存

muduo 的经典解法是：

> 常驻 Buffer 做小，但在每次读取时，临时再借一块足够大的栈空间，和 Buffer 的 writable 区一起交给 `readv`。

Tudou 当前实现正是这个思路。

### 3.2.2 当前实现的读取逻辑

`Buffer::read_from_fd()` 的核心逻辑可以概括成下面这段伪代码：

```cpp
char extraBuf[65536];
size_t writableBytes = writable_bytes();

iovec vec[2];
vec[0] = { buffer_.data() + writeIndex_, writableBytes };
vec[1] = { extraBuf, sizeof(extraBuf) };

int cnt = (writableBytes < sizeof(extraBuf)) ? 2 : 1;
ssize_t n = readv(fd, vec, cnt);
```

这里的关键点有四个：

#### 第一，`readv` 是 scatter/gather IO

它允许一次系统调用把数据分散写入多个用户态缓冲区。

这里传给内核的是两段目标地址：

1. Buffer 当前尾部的 writable 区
2. 栈上的 `extraBuf[65536]`

所以内核会按顺序先填满第一段，再继续写第二段。

#### 第二，栈上 `extraBuf` 是临时借用，不是每连接常驻

这是整个设计省内存的关键。

`extraBuf` 的生命周期只存在于这一次 `read_from_fd()` 调用栈里：

- 连接空闲时，它根本不占连接对象的常驻内存
- 只有真的进入读取路径，才临时在当前线程栈上拿出 64 KB 作为“突发流量缓冲垫”

所以它解决的是：

> 我不想为每个连接常驻准备 64 KB，但我希望在这次读取时又具备接近 64 KB 的吸收能力。

#### 第三，只有在真正溢出时，才把 `extraBuf` 的尾部 append 回 Buffer

`readv` 返回后有三种情况：

1. `n < 0`
   读取失败，记录 `errno`

2. `n <= writableBytes`
   说明这次数据全部落在 Buffer 自己的 writable 区里，只要推进 `writeIndex_`

3. `n > writableBytes`
   说明 Buffer 自己的尾部空间已经被填满，多出来的部分落进了栈上的 `extraBuf`

这时当前实现会：

```cpp
writeIndex_ = buffer_.size();
write_to_buffer(extraBuf, n - writableBytes);
```

也就是说，**只把真正溢出的那一截数据追加回 Buffer。**

这很重要，因为它说明：

- 大多数时候根本不会扩容
- 只有这次真实流量超过了常驻 Buffer 的 writable 区，才按需增长

#### 第四，如果当前 Buffer 尾部已经很大，就没必要再带第二块 iovec

源码里有一个小优化：

```cpp
const int cnt = (writableBytes < sizeof(extraBuf)) ? 2 : 1;
```

如果当前 Buffer 的 writable 区本来就已经不小于 64 KB，那么单独用 `vec[0]` 就够了，连第二块 `extraBuf` 都不必带上。

这说明当前实现不是机械地“每次都两段读”，而是按实际空间动态选择。

### 3.2.3 这种读法到底解决了什么？

它同时解决了三个问题：

#### 1. 降低每连接常驻内存

每个 Buffer 初始只有约 1 KB 的数据区，而不是 64 KB。

对大量低活跃连接来说，内存账会好看很多。量级上看，如果一个连接只有两个 1 KB 级 Buffer，那么 10000 个连接的常驻字节区只是几十 MB 量级，而不是动辄接近 1 GB。

#### 2. 常见情况下只需要一次系统调用

一次 `readv` 就同时利用了：

- Buffer 自己尾部已有的空间
- 栈上的临时大块空间

因此很多突发报文可以一次调用就读回来，不必“先看还剩多少空间，再扩容，再读一次”。

#### 3. 不必提前做 `ioctl(FIONREAD)` 或盲目 `reserve`

有些实现会先问内核“现在大概有多少字节可读”，然后提前 `reserve()` 一大片空间。

而 `readv + extraBuf` 的好处是：

- 不需要额外一次 `ioctl`
- 不需要在真正读取前猜测这次会来多少数据
- 先读回来，再按事实决定是否扩容

这就是一种很实用的工程思维：

> 不提前为极端情况长期付费，只在极端情况真的发生时短暂借资源。

---

## 3.3 写路径为什么也需要 Buffer？

很多人容易只关注输入缓冲区，却忽略输出缓冲区同样重要。

在非阻塞 IO 里，`write` / `send` 并不保证一次就把应用层想发的数据全部写入内核发送缓冲区。

常见情况有两种：

1. 只写出一部分，出现短写
2. 这次根本写不了，返回 `EAGAIN` / `EWOULDBLOCK`

如果没有应用层写缓冲区，业务层就必须自己维护“还剩多少字节没发、下次从哪里继续发”，会非常混乱。

Tudou 原有做法是：

1. `TcpConnection::send_in_loop()` 先把待发送数据 append 到 `writeBuffer_`
2. 然后让 `Channel` 关注 `EPOLLOUT`
3. 连接变得可写时，`TcpConnection::on_write()` 调 `writeBuffer_->write_to_fd()` 把当前可读区刷到 fd
4. 如果还没写完，就保留剩余数据，等待下次可写事件
5. 只有当 `writeBuffer_` 被写空时，才关闭 `EPOLLOUT` 关注

这里有一个很重要的工程点：

> **不要长期无条件关注 `EPOLLOUT`。**

因为 socket 在大多数时候往往是“可写”的。如果你一直关注写事件，LT 下 `epoll_wait` 会不断把它返回给你，造成无意义唤醒。

所以正确做法是：

- 有待发送数据时才打开 `EPOLLOUT`
- 写空后立刻关闭 `EPOLLOUT`

这也是 `writeBuffer_` 与 Channel 事件管理配合的意义。

### 3.3.1 写路径 Scatter-Gather I/O (writev) 向量合并写入优化

当发送缓冲区中已经存在积压数据时，如果有新数据需要发送，直接调用 `write_to_buffer()` 追加拷贝会带来两个代价：
1. **用户态内存分配与拷贝开销**：新数据需要追加入 `std::vector`，可能引发扩容和元素拷贝。
2. **多余的系统调用与事件驱动成本**：本来可以直接发送的数据，不得不全堆入缓冲区等待下次可写事件触发，拉长了数据路径。

为了解决该痛点，Tudou 引入了基于 `writev` 的 Scatter-Gather I/O 优化：

* **核心思路**：将 `writeBuffer_` 中的积压数据（通过已暴露为公有的 `readable_start_ptr()` 获取指针）和新发送的 `msg` 分别作为两段独立的向量 `iovec` 传入 `::writev` 写入套接字：
  ```cpp
  struct iovec iov[2];
  iov[0].iov_base = const_cast<char*>(writeBuffer_->readable_start_ptr());
  iov[0].iov_len = oldLen;
  iov[1].iov_base = const_cast<char*>(msg.data());
  iov[1].iov_len = msg.size();
  ssize_t n = ::writev(connSocket_.fd(), iov, 2);
  ```
* **零拷贝读游标推进 (`advance_read_index`)**：为了规避 `writev` 写入成功后消费缓冲区数据时，调用原有 `read_from_buffer(len)` 接口产生临时的 `std::string` 堆内存拷贝与分配，我们在 `Buffer` 中专门扩展了公有接口 `advance_read_index(size_t len)`：
  ```cpp
  void Buffer::advance_read_index(size_t len) {
      assert(len <= readable_bytes());
      maintain_read_index(len); // 仅前移读游标，0 拷贝，0 分配
  }
  ```
* **写流控状态机的细粒度处理**：
  * **完全发完**：如果写入字节数 `bytesWritten >= oldLen + msg.size()`，旧缓冲与新数据全部发送成功，直接关掉 `EPOLLOUT` 关注，并触发 `handle_write_complete_callback()`。
  * **旧缓冲发完，新数据部分发完**：清空旧缓冲，并将新数据未发完的部分追加到 `writeBuffer_` 中。
  * **旧缓冲仅部分发完，新数据完全没发**：仅推进旧缓冲的已发字节数游标，新数据整体追加到 `writeBuffer_` 中，继续注册 `EPOLLOUT` 等待下一次 Reactor 可写回调刷走。

---

## 3.4 为什么当前实现更适合 LT（水平触发）？

这部分是最容易在面试里说乱的地方。

先给结论：

> **不是因为 LT “更高级”，而是因为当前读取策略是“每次可读事件只读一次”，这和 LT 的语义天然匹配。**

### 3.4.1 当前仓库里有哪些直接证据说明它是 LT？

有三条直接证据：

#### 证据一：没有使用 `EPOLLET`

`Channel` 只定义了：

- 读事件：`EPOLLIN | EPOLLPRI`
- 写事件：`EPOLLOUT`

`EpollPoller::update_channel()` 直接把 `channel->get_events()` 写进 `epoll_event.events`，没有额外 OR 上 `EPOLLET`。

epoll 默认就是 LT，所以这里就是 LT。

#### 证据二：`TcpConnection::on_read()` 每次事件只读一次

当前读路径是：

```text
Channel::handle_events()
  -> TcpConnection::on_read()
     -> Buffer::read_from_fd()
```

`on_read()` 内部只调用了一次 `readBuffer_->read_from_fd(...)`，并没有写成：

```cpp
while (true) {
    read(...);
    if (errno == EAGAIN) break;
}
```

这说明当前设计本来就不是“每次事件把内核缓冲区彻底榨干”的 ET 风格，而是“本轮先读一次，然后尽快把控制权还给 EventLoop”的 LT 风格。

#### 证据三：`TimerQueue` 代码里直接写了 LT 语义

`TimerQueue::read_timerfd()` 的注释明确写到：

> timerfd 到期后变为可读，必须 read 消费事件，否则 epoll 会反复通知（LT 模式）

这说明当前仓库对 epoll 触发语义的假设就是 LT。

### 3.4.2 为什么 LT 下“只读一次”是正确的？

因为 LT 的语义是：

> 只要 fd 仍然处于可读状态，后续 `epoll_wait` 还会继续返回它。

所以，如果这次 `readv` 没有把内核接收缓冲区完全读空，也不会丢数据：

- 本轮先读一批
- 把读到的数据交给上层消息处理逻辑
- 事件循环去处理别的 ready fd
- 下一轮 `epoll_wait` 里，这个连接如果仍然可读，还会再次被返回

这就是 LT 的价值：**它允许你把“是否这次必须读干净”从正确性问题，变成调度策略问题。**

### 3.4.3 为什么这对低延迟和公平性有利？

如果某个热点连接当前积压了很多数据，而你在一次回调里一直循环 `read` 直到 `EAGAIN`，会发生什么？

- 这个连接会在本轮事件处理中占用更久 CPU
- 其他已经 ready 的连接需要继续等
- 应用层消息处理也更容易被单个大流量连接拖慢

而当前策略是：

1. 一次事件只做一次 `readv`
2. 常见情况下，借助 `extraBuf`，这一读已经能拿回不少数据
3. 然后立刻回到 EventLoop，让别的连接也有机会处理

所以它更像一种调度取舍：

> 我不追求“一个连接本轮尽可能读到最干净”，而追求“所有 ready 连接都尽快得到一次处理机会”。

这对追求低尾延迟的事件驱动程序通常是很合理的。

### 3.4.4 为什么 LT 不等于“可以用阻塞 fd”？

这是一个很常见的误区。

LT 只是说明：**你没读干净，后面还会继续通知。**

但它并不意味着：

- fd 可以安全地保持阻塞
- 事件循环线程可以接受某次 `read` 卡住

当前 Tudou 的实现明确使用非阻塞 socket，这非常关键。原因是：

1. 从 `epoll_wait` 返回到真正执行 `readv` 之间，状态可能已经变了
2. 非阻塞 fd 在“现在读不了”时返回 `EAGAIN`，而不是把 loop 卡住
3. 整个 Reactor 模型要求一个线程管理多个连接，不能让某个 fd 把线程睡死

所以当前仓库采用的是：

> **LT + 非阻塞 fd + 每次事件只读一次**

这三者是一套配合关系，不要拆开理解。

---

## 3.5 为什么当前代码如果换成 ET，反而会出问题？

这能帮助你真正理解 LT 的必要性。

ET 的语义是：

> 只有从“不就绪”变成“就绪”的那一瞬间，你才会收到通知。

如果当前代码不改，只是简单把 epoll 事件改成 ET，那么：

1. 某次可读事件来了
2. `TcpConnection::on_read()` 只做一次 `readv`
3. 内核接收缓冲区里其实还剩下一部分数据
4. 但因为状态仍然是“可读”，没有新的边沿变化
5. 后面可能收不到新的读事件
6. 剩余数据就可能一直躺在内核缓冲区里，连接表现得像“卡住了”

这就是为什么 ET 常见写法必须是：

```cpp
while (true) {
    ssize_t n = read(fd, buf, len);
    if (n > 0) {
        // continue
    } else if (n == 0) {
        // peer closed
        break;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
    } else {
        // error
        break;
    }
}
```

也就是说，**ET 要求你在本轮回调里尽量把状态“读到不可再读”为止。**

这和当前 Tudou 的策略正好相反。

### 3.5.1 ET 不一定更高效

很多人会机械地认为：

> ET 通知更少，所以一定更快。

这不一定成立。

如果 ET 迫使你：

- 每次可读都循环读到 `EAGAIN`
- 平均多做若干次 `read`
- 某些热点连接长期占用 loop

那么省下来的通知次数，未必能抵掉增加的系统调用和调度不公平成本。

所以 LT 和 ET 不是“谁先进谁落后”的关系，而是：

- LT 更宽容，代码更容易写对，调度更灵活
- ET 更苛刻，通知更少，但要求你把 drain 逻辑写得非常完整

当前 Tudou 这套 Buffer 设计与读事件策略，更自然地站在 LT 这一侧。

---

## 4. Result（结果）

把上面的设计组合起来，最终得到的是一套很实用的工程折中：

### 4.1 内存上：默认小，按需长

- 每个 Buffer 初始只有 1 KB 级常驻空间
- 连接空闲时不会背着大块缓冲区常驻内存
- 真遇到一次突发读流量时，用栈上 `extraBuf` 临时顶住
- 只有真的溢出到常驻 Buffer 不够了，才扩容

这让“低活跃连接很多”的场景更划算。

### 4.2 系统调用上：常见路径更短

- 一次 `readv` 同时利用两段用户态空间
- 很多场景里一次可读事件只要一次系统调用就能把大部分数据拿回
- 不需要先 `ioctl(FIONREAD)` 再决定预留空间

这让读路径更直接。

### 4.3 正确性上：天然适配非阻塞短读短写

- 读不到返回 `EAGAIN` 是正常控制流
- 写不完的数据留在 `writeBuffer_`，后续靠 `EPOLLOUT` 继续刷
- 读空时复位索引，空间可复用
- 空间搬移用 `memmove`，避免重叠拷贝问题

这让 Buffer 成为稳定的“字节状态机”。

### 4.4 调度上：更偏向低延迟和公平性

- 每次可读事件只读一次，不把一个连接一次性榨干
- LT 负责保证“没读完也不会丢通知”
- EventLoop 能更快轮到其他 ready 连接

这让系统整体的调度行为更平衡。

### 4.5 写路径上通过 writev 减少数据拷贝与系统调用

- **零拷贝合并发送**：在有积压时使用 `writev` 将缓冲区数据和新消息合并为单次系统调用发送，减少了频繁 write 的开销，且避免了用户态的大字符串拼接和动态扩容。
- **游标指针移动**：新增 `advance_read_index` 方法，避免了缓冲区推进时拷贝产生的临时对象，保持写入与清除路径的轻量化。

---

## 5. 面试时怎么讲

如果面试官问你：

> 你们的 Buffer 是怎么设计的？为什么要用 `readv`？为什么选 LT？

你可以按下面这个顺序回答。

### 5.1 30 秒版本

在非阻塞 Reactor 里，应用层 Buffer 主要用来承接内核 socket 缓冲区和上层协议之间的状态，处理半包、短写和收发进度。Tudou 的 Buffer 用一个连续数组和读写索引维护 prepend、readable、writable 三段区域，输入路径参考 muduo：常驻 Buffer 默认只做成 1 KB 级别，但在 `read_from_fd()` 里会额外借一块 64 KB 栈上 `extraBuf`，用一次 `readv` 同时读入两段内存。这样既避免每连接常驻大缓冲区，又常常能在一次系统调用里拿回足够多数据。在写入路径上，Tudou 支持基于 `writev` 的 Scatter-Gather I/O 优化，在有积压时直接利用两段向量将积压缓冲区与新数据一起发出，并配合 `advance_read_index` 零拷贝向前移动读游标，消除用户态字符串拼接及堆内存拷贝开销。当前仓库没有打开 `EPOLLET`，所以是 LT；而 `TcpConnection::on_read()` 每次事件只读一次，这和 LT 很匹配，因为只要内核里还有未读数据，后续 epoll 还会继续通知。这样做的好处是低延迟、公平性更好，避免热点连接在一次回调里把 loop 长时间占住。

### 5.2 2 分钟版本

如果想讲得更完整，可以分四层：

1. **先讲问题**
   非阻塞网络编程里不能假设一次 `read` 就得到完整消息，也不能假设一次 `write` 就把数据发完，所以需要应用层 Buffer 保存连接的读写进度。

2. **再讲结构**
   Buffer 用 `std::vector<char>` 做底层存储，`readIndex_` 和 `writeIndex_` 把内存划成 prepend、readable、writable 三段；读取推进读指针，写入推进写指针，空间不足时优先复用头部空洞，不够再扩容。

3. **再讲输入优化**
   常驻 Buffer 不做大，而是在每次 `read_from_fd()` 里放一个 64 KB 的栈上 `extraBuf`，然后用一次 `readv` 同时往 Buffer writable 区和 `extraBuf` 里读。这样平时省内存，流量突发时又不必为了多读一点反复系统调用。

4. **最后讲 LT 的原因**
   当前 `on_read()` 每次事件只读一次，这要求 epoll 使用 LT 而不是 ET。因为 LT 下没读完的数据还会继续触发读事件，所以一次不读空没问题；这样可以减少单个连接对事件循环的占用，提高公平性和低延迟表现。若换成 ET，就必须循环读到 `EAGAIN`，否则容易漏掉剩余数据。

### 5.3 高频追问与标准回答

#### 追问一：既然内核已经有 socket 缓冲区，为什么还要应用层 Buffer？

因为内核缓冲区不帮你维护应用层的消息边界、短写续传和业务消费进度。应用层 Buffer 是连接状态的一部分。

#### 追问二：为什么不直接给每个连接分配一个很大的 Buffer？

因为高并发场景里大多数连接并不活跃，大常驻 Buffer 会把内存浪费在闲置连接上。`readv + 栈上 extraBuf` 的意义就是让“平时小、突发时大”。

#### 追问三：为什么只读一次不会丢数据？

因为当前是 LT。只要 fd 还可读，后续 `epoll_wait` 还会继续返回它，所以“一次不读完”不会破坏正确性。

#### 追问四：那为什么还要把 fd 设成非阻塞？

因为 LT 只解决“通知会不会再来”，不解决“这次调用会不会阻塞线程”。Reactor 里一个线程要管很多连接，所以 fd 仍然必须非阻塞，读不到就返回 `EAGAIN`，不能把 loop 卡住。

#### 追问五：ET 一定比 LT 快吗？

不一定。ET 通知更少，但要求你每次回调把 fd 尽量 drain 到 `EAGAIN`，否则会漏事件。这样可能增加系统调用次数，也可能让热点连接更长时间占住事件循环。是否更快要看整体调度和实现质量，不能只看通知次数。

---

## 6. 一句话总结

**Tudou 当前的 Buffer 设计，本质上是在做四件事：**

1. 用小常驻内存保存连接状态；
2. 用 `readv + 栈上 extrabuf` 吸收突发读流量，减少读系统调用；
3. 用 `writev` 配合 `advance_read_index` 在有积压时合并发送新旧数据，减少写系统调用并杜绝用户态拷贝；
4. 用 LT 保证“每次事件只读一次”仍然正确，从而把调度重点放在低延迟和公平性上。

如果把它压成一句最适合面试的话，可以这么说：

> 我们让连接的常驻 Buffer 保持初始 1 KB 大小以节省内存，当发生突发大流量读时，临时借用 64 KB 的栈上空间配合 `readv` 单次系统调用读取。在写路径上有积压时，引入 `writev` 将缓冲区残留与新发送数据合并发出，并通过 `advance_read_index` 零拷贝移动缓冲区读游标以规避用户态的拼接拷贝。整套系统在 LT 水平触发机制下运行，“每次可读只读一次”，在保证吞吐率的同时，大幅提升了多连接调度的公平性并降低了尾延迟。

---

## 7. 对照源码阅读时，建议重点看这几处

- `src/tudou/tcp/Buffer.h`
  Buffer 的抽象模型、索引语义和公开接口

- `src/tudou/tcp/Buffer.cpp`
  `read_from_fd()` 的 `readv + extraBuf`、`make_space()` 的复用与扩容逻辑、`write_to_fd()` 的写出逻辑

- `src/tudou/tcp/TcpConnection.cpp`
  `on_read()` 为什么每次只读一次，`send_in_loop()` / `on_write()` 如何配合 `writeBuffer_`

- `src/tudou/tcp/Channel.cpp`
  注册的事件类型里没有 `EPOLLET`

- `src/tudou/tcp/EpollPoller.cpp`
  事件如何被原样写入 `epoll_ctl`

- `src/tudou/tcp/Socket.cpp`
  监听 socket 与连接 socket 都是非阻塞创建

- `src/tudou/tcp/TimerQueue.cpp`
  注释里直接体现了当前仓库对 LT 语义的依赖

把这些文件串起来看，你对 Buffer、LT、非阻塞 IO、调度公平性之间的关系就会非常清楚。