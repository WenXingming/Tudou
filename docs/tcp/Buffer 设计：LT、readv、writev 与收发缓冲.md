# Buffer 设计：LT、readv、writev 与收发缓冲

`Buffer` 是 TCP 层的字节工具：它承接非阻塞 I/O 的短读、短写和应用层未消费字节，但不理解 HTTP、RPC 或任何业务协议。

# Situation — 情境

内核 socket 缓冲区只保存内核态字节，不知道协议边界，也不会替应用保存“本次没有写完的数据”。非阻塞 I/O 中，读到的字节可能只是一条消息的一部分，写入也可能只接受部分数据。

因此每条 `TcpConnection` 需要：

- `readBuffer_`：保存已经从 fd 取到、但业务尚未消费的字节；
- `writeBuffer_`：保存业务要发送、但内核尚未完全接收的字节。

同时，缓冲区不能为每个连接预分配很大内存：大量空闲连接会浪费常驻内存；但突发输入也不能通过反复扩容和多次 read 降低吞吐。

# Task — 任务

Buffer 需要：

1. 用连续内存保存可读字节，并支持高效顺序读写；
2. 小流量时保持较小常驻内存；
3. 一次读取时尽可能吸收突发数据，避免“先扩容再 read”；
4. 在空间不足时优先复用已消费区域，再决定扩容；
5. 正确衔接 `readv`、`writev`、短写和 `EAGAIN`。

# Action — 设计与实现

## 连续数组与两个索引

Buffer 用 `std::vector<char>` 和 `readIndex_`、`writeIndex_` 划分三段区域：

```text
+-------------------+------------------+------------------+
| prependable bytes |  readable bytes  |  writable bytes  |
+-------------------+------------------+------------------+
0               readIndex_         writeIndex_        size
```

```cpp
size_t Buffer::readable_bytes() const { return writeIndex_ - readIndex_; }
size_t Buffer::writable_bytes() const { return buffer_.size() - writeIndex_; }
```

初始可写空间为 1 KiB，开头保留 8 字节 prepend 区。当前 Buffer 未向上层暴露 prepend 接口；这段空间的当前作用是让数据回收后保持统一的索引起点，而不是宣称项目已经实现了某种协议包头零拷贝。

## readv：主缓冲区加 64 KiB 栈缓冲

读 fd 时，Buffer 把现有可写区和栈上的临时区同时传给 `readv`：

```cpp
char extraBuf[kStackBufSize];
const size_t writableBytes = writable_bytes();

iovec vec[2];
vec[0] = {buffer_.data() + writeIndex_, writableBytes};
vec[1] = {extraBuf, sizeof(extraBuf)};
const ssize_t n = ::readv(fd, vec, writableBytes < sizeof(extraBuf) ? 2 : 1);

if (n <= writableBytes) {
    writeIndex_ += n;
} else {
    writeIndex_ = buffer_.size();
    write_to_buffer(extraBuf, n - writableBytes);
}
```

常见小包直接写进现有 vector；突发大包的溢出部分先进入栈上 64 KiB 临时区，确认实际读到多少后才扩容并追加。这避免了为了“可能到来的大包”而让每个连接长期占大缓冲区，也避免“先猜测大小扩容、再 read”的额外动作。

## writev：直接发送积压字节与新消息

`writev` 不属于 Buffer 的成员函数，而是 `TcpConnection::send_in_loop()` 使用 Buffer 的可读区完成的发送优化。已有待发送字节时，它把 Buffer 中的旧数据和本次新消息组成两个 iovec：

```cpp
iovec iov[2];
iov[0] = {const_cast<char*>(writeBuffer_.readable_start_ptr()), oldLen};
iov[1] = {const_cast<char*>(msg.data()), msg.size()};
const ssize_t n = ::writev(connSocket_.fd(), iov, 2);
```

这样无需先把新消息复制进 `writeBuffer_` 再整体发送：内核按 iovec 顺序读取“旧积压 → 新消息”，保证字节顺序；只有未写出的新消息才追加进 Buffer，等待下一次 `EPOLLOUT`。

## 空间回收优先于扩容

当尾部空间不足时，先检查前部已消费区域是否足够容纳数据：

```cpp
const size_t available = writable_bytes() + prependable_bytes();
if (available < len + kCheapPrepend) {
    buffer_.resize(writeIndex_ + len);
    return;
}

std::memmove(buffer_.data() + kCheapPrepend,
             readable_start_ptr(), readableBytes);
readIndex_ = kCheapPrepend;
writeIndex_ = readIndex_ + readableBytes;
```

只有复用旧空间仍不足时才扩容。移动区域可能重叠，因此使用 `memmove`；数据全部被消费后，两个索引直接复位，不必释放和重新分配 vector。

## 与 TcpConnection 的协作

读路径把 `read_from_fd()` 的结果交给上层：`> 0` 触发消息回调，`0` 表示 EOF，`EAGAIN` 直接返回。写路径由 TcpConnection 选择直写或 `writev`；Buffer 只保存未发送字节，并在可写事件到来后通过 `write_to_fd()` 继续发送。

当前 Channel 没有设置 `EPOLLET`，因此使用默认水平触发。一次 read 没有读空内核缓冲区时，fd 仍可读，后续 poll 会再次通知；这使“每次可读事件读取一次”的当前策略成立。若改为 ET，必须改为循环读到 `EAGAIN`，不能只改 epoll 标志。

# Result — 结果

- 应用层可以保存半包、粘包中的未消费字节和短写残留；
- 每连接的常驻初始空间较小，突发输入由 readv 栈缓冲吸收；
- 已消费区域优先复用，减少不必要扩容；
- Buffer 只处理字节与系统调用边界，协议解析仍在上层；
- LT 语义、非阻塞返回值和 Buffer 状态形成一致的收发路径。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 双 Buffer | 读缓冲保存未消费输入，写缓冲保存未发送输出。 |
| 连续内存 | 两个索引实现顺序读写，读空后直接复位。 |
| readv | 现有可写区加栈上临时区，一次吸收常见突发输入。 |
| writev | 复用 Buffer 可读区，将旧积压与新消息按顺序一次交给内核。 |
| 空间策略 | 先移动复用前部空洞，仍不足才扩容。 |
| 事件语义 | 当前为 LT；切换 ET 必须同时改变读循环策略。 |

# 面试核心问答总结

## Q1：内核已有 socket 缓冲区，为什么还需要应用层 Buffer？

内核缓冲区不保存协议解析进度，也不替应用处理短写。应用层 Buffer 保存“已经读到但未消费”和“想发送但未写完”的字节状态。

## Q2：readv 加栈缓冲相比先扩容再 read 有什么优势？

小包直接进入主缓冲；突发数据暂时进入栈缓冲，只有实际读到溢出数据时才扩容。这样兼顾小常驻内存和较少的系统调用、内存操作。

## Q3：为什么先复用 prependable 区而不是立即 resize？

前部是已消费字节留下的可用空间。把剩余可读字节前移可以避免重新分配；只有总可用空间仍不够时才扩容。

## Q4：writev 在这里解决什么问题？

当 writeBuffer 已有积压时，writev 直接发送“旧缓冲 + 新消息”两段数据，避免先把新消息拷贝进 Buffer 再拼接发送；未发送部分才进入 Buffer。

## Q5：为什么当前 LT 模式可以每次只读一次？

LT 下只要内核接收缓冲区仍可读，后续 epoll_wait 会再次通知。ET 下只通知状态边沿，必须循环读到 `EAGAIN`，否则可能遗留数据却不再收到通知。
