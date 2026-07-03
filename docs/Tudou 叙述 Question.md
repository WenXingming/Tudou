## 你用 2 分钟介绍一下 Tudou

你用 2 分钟介绍一下 Tudou。不要背 README，也不要直接说“这是一个高性能 Reactor 网络框架”。请讲清楚：

    1. 这个项目解决什么问题？
    2. 你实现了哪些核心模块？
    3. 一次 HTTP 请求在框架里大概怎么走？
    4. 你认为这个项目最有技术含量的点是什么？

> [!tip]
>
> Tudou 是一个高性能的基于 Reactor 的网络框架，内部主要是基于 Tcp 协议，并实现了 Http 协议层。它主要解决两个问题：一是解决 C++ 下IO 多路复用 + 非阻塞IO 编程的复杂性，我们的框架采用了 Facade 门面设计模式，最终暴露的公开接口非常简洁易用；二是提升了IO编程的性能，只需一个参数即可启动多线程/线程池，经过测试其性能也比较高，比如说在我的工作站上（2.4GHZ，24核）其实可以达到百万 QPS 以上。

```
停。这个回答现在像简历摘要，不像你真的掌控了项目。

  主要问题：

  1. “高性能 Reactor 网络框架”太泛了，任何 muduo 仿写项目都能这么说。
  2. “基于 TCP，实现 HTTP 协议层”是对的，但你没有说出核心模块：EventLoop、EpollPoller、Channel、Acceptor、
     TcpConnection、TcpServer、Buffer、TimerQueue、HttpContext、Router。

  3. “只需一个参数启动多线程/线程池”说得太产品化，面试官会继续问：这个参数改变了什么？main loop 和 IO loop
     怎么分工？连接怎么分配？

  4. “百万 QPS 以上”要谨慎。README 里记录的 Tudou 多线程 hello benchmark 是约 871k req/s，muduo 是约 902k
     req/s。你可以说“接近百万 QPS”或“同数量级”，不要上来就说百万以上，除非你能拿出对应压测记录。

  5. 你完全没讲“一次请求怎么走”，这是面试官判断你是否真懂项目的关键。

  你可以改成这样开头：

  > Tudou 是我实现的一个 Linux C++14 多线程 Reactor 网络框架。底层用 epoll 做 I/O 多路复用，用 eventfd 做
  > 跨线程唤醒，用 timerfd 做定时任务，把 socket 读写、异步任务和定时器都统一到 EventLoop 里处理。TCP 层封
  > 装了 Acceptor、TcpConnection、TcpServer 和 Buffer，上层又基于 llhttp 实现了 HTTP 解析、路由分发和
  > HTTPS 支持。

  然后再讲：

  > 它解决的主要问题是把 Linux 非阻塞 I/O 的复杂细节封装掉，让业务只需要注册连接、消息、HTTP route 等回
  > 调。同时通过 one loop per thread 的模型，让每条连接固定归属于一个 IO loop，连接读写和状态管理都在同一
  > 线程内完成，减少锁竞争。
```



## 一个 HTTP 请求进来后，从 accept 到业务 handler，再到响应写回客户端，完整调用链是什么？

> [!tip]
>
> 首先 HTTP 请求底层基于 Tcp 请求，我们的服务器接收到 Tcp 请求后并建立 3 次握手后，会进入全连接队列，epoll 监听到监听套接字上有可读事件，经过事件循环收集到该活跃事件并进行事件分发、触发回调。回调接着会传递到上层 Tcp 层的 Acceptor，其会建立连接。我感觉有点混乱了，请你解释

```
• 你前半段方向是对的，混乱点在于：三次握手和全连接队列是内核做的；Tudou 从“监听 fd 可读”开始接管。 不要把内核 TCP 状态机和框架调用链混在一起讲。

  可以按这条线记：

  客户端发起连接
  -> 内核完成三次握手，把连接放入 accept 队列
  -> listen fd 在 epoll 中变为可读
  -> main EventLoop::loop()
  -> EpollPoller::poll()
  -> Channel::handle_events()
  -> Acceptor::on_read()
  -> Socket::accept()
  -> TcpServer::on_connect()
  -> 选择一个 IO EventLoop
  -> IO loop 中创建 TcpConnection
  -> TcpConnection 的 Channel 监听 conn fd 可读

  请求数据真正到来后：

  conn fd 可读
  -> IO EventLoop::loop()
  -> EpollPoller::poll()
  -> Channel::handle_events()
  -> TcpConnection::on_read()
  -> Buffer::read_from_fd()
  -> TcpServer::on_message()
  -> HttpServer::on_message()
  -> conn->receive()
  -> HttpContext::parse()
  -> Router::dispatch()
  -> handler(req, resp)
  -> HttpResponse 打包
  -> TcpConnection::send()
  -> send_in_loop()
  -> 直接 write 或写入 writeBuffer_ 并监听 EPOLLOUT
  -> TcpConnection::on_write()
  -> writeBuffer_ 刷回 fd

  更口语化一点，你面试可以这样讲：

  > 连接建立阶段由 main loop 负责。内核完成三次握手后，监听 fd 可读，Acceptor 的 Channel 被 epoll 唤醒，Acceptor::on_read() 调 accept 拿到连接 fd。然后 TcpServer::on_connect() 通过线程池选择一个 IO loop，把这个
  > fd 投递到对应线程，在那个线程里创建 TcpConnection，并给连接 fd 注册读事件。

  然后接：

  > 数据读写阶段由 IO loop 负责。请求数据到来后，连接 fd 可读，TcpConnection::on_read() 调 Buffer::read_from_fd() 把字节读进缓冲区，再触发 TcpServer::on_message()。HTTP 层的 HttpServer::on_message() 会调用
  > conn->receive() 取出字节，交给 HttpContext 基于 llhttp 解析。解析完整后通过 Router::dispatch() 找到业务 handler，生成 HttpResponse，最后调用 conn->send() 写回。如果一次写不完，就进入 writeBuffer_，等 EPOLLOUT
  > 再继续写。

  关键分界线：

  - Acceptor 只处理新连接
  - TcpConnection 处理已建立连接上的读写
  - main loop 通常负责 accept
  - IO loop 负责连接后续 I/O
  - HTTP 层不直接碰 fd，只处理从 TCP 层拿到的字节流
```



## 为什么新连接 accept 出来后，不直接在 main loop 里创建并处理 TcpConnection，而要分配给 IO loop？

> [!tip]
>
> 考虑到多线程的性能问题和职责分离，我们的 mainLoop 运行在主线程只负责新连接接收，具体的连接 IO 处理则被分给了 IO 线程。这样的好处是：1 是职责分离清楚，各个线程之间耦合程度低，减少多线程交集，简化并发时的条件竞争；2 是提升性能，避免 IO 逻辑影响主线程连接接收的性能

```
这个回答方向对，但还不够“项目级”。你需要把“为什么”和“怎么保证线程安全”讲出来，否则面试官会继续压。

  可以补成这样：

  > main loop 只负责监听 fd 和 Acceptor，新连接 accept 出来后通过 EventLoopThreadPool::get_next_loop() 选
  > 择一个 IO loop。真正的 TcpConnection 会在目标 IO loop 线程里创建，因为 Channel 构造和 epoll 注册要求发
  > 生在所属 EventLoop 线程。后续这个连接的读、写、关闭、心跳刷新也都在同一个 IO loop 内执行。

  这里有两个关键点：

  1. 不是单纯为了职责分离，而是线程归属约束。
     Channel 构造时会注册到对应 loop 的 EpollPoller，EventLoop::update_channel() 里要求
     assert(is_in_loop_thread())。所以连接 fd 分给哪个 IO loop，TcpConnection 和它的 Channel 就必须在那个
     线程里创建。

  2. 这样才能让连接状态无锁。
     每个连接固定归属于一个 IO loop，读缓冲、写缓冲、关闭状态、心跳刷新都只在这个线程里改。跨线程调用
     send() 或 force_close() 时，不直接操作连接，而是 queue_in_loop() 投递回所属 loop。

  更强的面试回答：

  > 如果直接在 main loop 里处理所有 TcpConnection，那 main loop 既要 accept，又要处理所有连接读写，会成为
  > 瓶颈。分配给 IO loop 后，main loop 只处理新连接接入，IO loop 并行处理连接 I/O。并且每条连接固定绑定一
  > 个 IO loop，连接内部状态只在所属线程访问，所以读写缓冲和连接状态不需要加锁。
```



## 你说“连接状态不需要加锁”。那 TcpServer 是怎么管理所有连接的？连接表的数据结构是什么？为什么它可以无锁？

> [!tip]
>
> TcpServer 通过一个哈希表管理所有连接。之前是一个单一的哈希表，多线程读写该哈希表时需要加锁，我意识到这把锁的粒度太大，应该会导致性能问题。因此我设计了一个双层哈希，通过巧妙设计实现了无锁并发。首先我们在初始化 TcpServer 时会初始化第一层哈希表，其 key 是 eventloop 指针，创建后多线程只会访问不会修改，因此可以无锁。其 value 也是一个哈希表（二层哈希），存储的是每个线程所属的 TcpConnection 连接，每个线程都只会修改自己所属的那个二层哈希表。
>
> 总之，我们形成了这样一个哈希结构，一层哈希在初始化 Tcpserver 时创建，不会再被修改，只会访问所以无锁。它的每个桶存放的都是单个线程的连接，只会由该线程读写，也是无锁。因此整个结构无需加锁。

```
• 这次回答明显好很多，基本过关。你讲清楚了核心结构和无锁前提。

  但有三处要修正/增强：

  1. 不是“初始化 TcpServer 时”创建外层哈希，而是 TcpServer::start() 里，EventLoopThreadPool::start() 之后拿到所有 loops，再初始化 connectionRecordsByLoop_。构造函数只记录配置，不创建运行时资源。
  2. 外层不是“每个桶存放单线程连接”，更准确是：外层 unordered_map<EventLoop*, ConnectionRecords>，每个 EventLoop* 对应一个 ConnectionRecords。
  3. value 里不是只存 TcpConnection，而是 ConnectionRecord，包含 TcpConnectionPtr connection 和可选的 ConnectionHeartbeat。

  你可以这样答得更精确：

  > TcpServer 里是 unordered_map<EventLoop*, ConnectionRecords> connectionRecordsByLoop_。外层 map 在 start() 阶段初始化，每个 EventLoop 对应一个空的连接记录表，运行期不再插入或删除外层 key，所以多线程只做 find。
  > 内层 ConnectionRecords 是 unordered_map<TcpConnection*, ConnectionRecord>，保存连接本身和可选心跳对象。每个内层表只由对应 EventLoop 线程访问，比如创建连接、关闭连接、刷新心跳都在所属 IO loop 里执行，因此不需
  > 要锁。

  然后补一句边界：

  > 这里无锁成立的前提是严格遵守 one loop per thread，跨线程操作必须通过 queue_in_loop() 投递回连接所属 loop。如果直接从其他线程改连接表，这个设计就不安全。
```



## 既然连接表无锁，那项目里为什么 EventLoop 的 pendingFunctors_ 还需要 mutex？这和你说的无锁是不是矛盾？

> [!tip]
>
> 连接表无锁是 TCP Server 类里的，其管理所有的 TCP 连接。pendingFunctors_ 是跨线程投递的，每一个线程（Event Loop）都有一个 pendingFunctors_ 用来接收其他线程投递过来的任务，这里可能会产生并发竞争，例如多个线程向同一个线程投递任务，因此这里需要加锁。

```
• 对，这题回答过关。你抓住了边界：连接表的写入者被限制为一个所属 loop 线程；任务队列的生产者可能是多个线
  程。

  可以再压实一点：

  > 无锁不是全项目无锁，而是特定数据结构在特定访问模式下不需要锁。ConnectionRecords 的写入线程唯一，所以无
  > 锁；但 pendingFunctors_ 是 MPSC 场景，多个外部线程都可能向同一个 EventLoop 投递任务，必须用
  > pendingFunctorsMutex_ 保护队列。

  再补一个实现细节会更强：

  > 执行 pending functors 时不是一直拿着锁执行，而是在 do_pending_functors() 里把队列 swap 到局部变量，马
  > 上释放锁，然后逐个执行。这样避免回调执行期间阻塞其他线程继续投递任务，也避免任务执行逻辑和锁耦合。
```



## 生命周期深水区：Channel::tie_to_object() 是干什么的？如果没有 tie，TcpConnection 在什么场景下可能出问题？

> [!tip]
>
> 主要是为了延长对象的生命周期。我们的对象由 shared_ptr 管理，当事件循环捕捉到活跃 IO 事件并触发回调后，这里有一个问题是事件可能是连接关闭事件，因此回调来到上层时可能消耗 TcpConnection 对象，TcpConnection 拥有底层 Channel，负责其生命周期管理，如果 TCP Connection 被消耗就代表 Channel 也会被同步销毁，但是回调触发还是在底层 Channel 类里被触发的，回调返回时还是要回到 Channel、并在 Channel 里可能要处理后续逻辑。
>
> 我们要确保回调处理期间 Channel 不能被销毁，也就是拥有它的 TcpConneciton 不能被销毁，因此通过 weak_ptr 提升为 shared_ptr，增加 TcpConnection 引用计数，确保其在回调执行期间不会被销毁，保证内存访问安全。

```
• 这题答得不错，核心意思对：tie 是为了防止回调期间 TcpConnection 被析构，进而导致 Channel 悬空。

  但你有两个措辞要改：

  1. 不要说“消耗 TcpConnection 对象”。面试里听起来不清楚。说“释放最后一个 shared_ptr”或“引用计数归零导致析
     构”。

  2. tie 不是“延长对象生命周期”这么泛，而是在 Channel::handle_events() 本轮回调栈期间临时保活 owner。它不
     改变长期所有权。

  更精确回答：

  > TcpConnection 由 shared_ptr 管理，但 Channel 的回调里捕获的是 this。当 epoll 返回某个连接 fd 的事件
  > 后，调用链会进入 Channel::handle_events()，再触发 TcpConnection::on_read/on_close/on_error。如果这些回
  > 调继续调用用户回调，用户回调里可能关闭连接，导致 TcpServer 从连接表删除该连接。如果这是最后一个
  > shared_ptr，TcpConnection 会析构，内部的 Channel 也会析构。但此时执行栈还在 Channel::handle_events()
  > 里，后续访问就可能 use-after-free。

  > 所以 TcpConnection::create_connection() 后会 channel_->tie_to_object(conn)。Channel 内部保存
  > weak_ptr<void>，每次 handle_events() 开始时先 lock() 成临时 shared_ptr。只要 lock 成功，本轮事件分发期
  > 间 owner 就不会析构；如果 lock 失败，说明 owner 已经没了，直接跳过回调。
```

 

## 为什么这里保存的是 weak_ptr，不是让 Channel 直接持有 shared_ptr<TcpConnection>？

> [!tip]
>
> 这是一个所有权的问题。从设计上一般是上层拥有下层，保证单向依赖，如果 Channel 也拥有 TcpConnection，并且也是 Shared_ptr ，那么就可能导致循环依赖。weak_ptr 非常合适，不拥有但是保留观察能力。

```
• 对，核心正确。你可以再说得更具体一点：

  > TcpConnection 拥有 Channel，如果 Channel 再用 shared_ptr 拥有 TcpConnection，就形成强引用环：
  > TcpConnection -> Channel -> TcpConnection。这样即使 TcpServer 从连接表里删掉连接，引用计数也不会归零，
  > 连接和 fd 相关资源都泄漏。weak_ptr 不增加引用计数，只在事件分发入口临时 lock() 成 shared_ptr，所以既能
  > 回调期间保活，又不改变长期所有权关系。

```

 

## Channel 析构时做了什么？它会不会 close fd？为什么 TcpConnection 里 connSocket_ 要声明在 channel_ 前面？

> [!tip]
>
> Channel 析构时会 disable_all，也就是该 fd 不再对任何事件感兴趣，不会被 epoll 收集，同时其析构时也会将自己从 poller 中删除掉，这里的删除包括从 poller 的管理结构 map 删除和从 epoll 里通过 epoll_ctl 删除，因此 epoll 不再监听该 fd。

```
这一半答对了，但题目还有两个关键点你没答：

  1. Channel 不 close fd。
     它只负责 epoll 注册和事件分发。fd 的所有权在 Socket，Socket 析构时 close。

  2. connSocket_ 声明在 channel_ 前面，是为了析构顺序。
     C++ 成员析构顺序和声明顺序相反。TcpConnection 里：

  Socket connSocket_;
  std::unique_ptr<Channel> channel_;

  所以析构时先析构 channel_，从 epoll 注销；再析构 connSocket_，关闭 fd。

  这很重要，因为如果先 close fd，再让 epoll 里还保留这个 fd 对应的 Channel*，就可能出现悬空事件，甚至 fd
  被系统复用后导致更难排查的问题。

  合格答案应该是：

  > Channel::~Channel() 会先 disable_all()，把感兴趣事件清空并同步给 Poller，然后 remove_in_register()，由
  > Poller 从内部 map 和 epoll interest list 中删除这个 fd。它不会关闭 fd，fd 由 Socket RAII 管理。
  > TcpConnection 中 connSocket_ 声明在 channel_ 之前，是为了让析构时 channel_ 先执行 epoll 注销，随后
  > connSocket_ 再 close fd，保证 epoll 不会持有已销毁 Channel 或已关闭 fd 对应的悬挂状态。
```



## TcpConnection::send() 如果一次写不完，具体怎么处理？为什么不能一直监听 EPOLLOUT？

> [!tip]
>
> 为了提高性能，send 的流程是先直接 write_to_fd，如果能一次性直接完全写入内核缓冲区，send 就完成了。如果不能，会把剩余数据写入 writeBuffer，并通过 enable_writing 标志该 fd 可写，因此 epoll 事件循环会触发写事件然后事件分发、回调，在回调函数中将 buffer 数据继续写入内核缓冲区。缓冲区为空时 disable_writing，如果一直监听 EPOLLOUT，就会导致 epoll 收集触发回调但是 buffer 为空的错误路径

```
• 这题基本过关，但最后一句要改。

  不是“buffer 为空的错误路径”，而是：非阻塞 socket 大多数时候都是可写的。如果一直监听 EPOLLOUT，水平触发
  epoll 会频繁返回这个 fd，即使当前没有待发送数据，也会不断触发写回调，造成无意义唤醒和 CPU 空转。

  更准确说法：

  > send() 如果不在连接所属 loop 线程，就先投递回 loop。send_in_loop() 里，如果当前写缓冲为空且没有监听写
  > 事件，会先尝试直接 write 到 socket。能一次写完就触发 writeCompleteCallback 并返回。写不完或者内核发送
  > 缓冲区暂时满了，就把剩余数据追加到 writeBuffer_，再 enable_writing() 关注 EPOLLOUT。等 socket 可写时，
  > TcpConnection::on_write() 调 writeBuffer_->write_to_fd() 继续发送。发送缓冲清空后立刻
  > disable_writing()，避免 epoll 在 fd 一直可写时反复唤醒。

  再补一个加分点：

  > 追加写缓冲后会检查 highWaterMark_。如果缓冲大小从低于阈值变成超过阈值，会触发高水位回调，让上层做限流
  > 或暂停读取。
```

 

## Buffer::read_from_fd() 为什么用 readv，而不是直接 read 到 vector 里？这个设计解决了什么问题？

> [!tip]
>
> 这个主要是既使得每个连接的 Buffer 不会过于大，过于占用内存，我们每个连接有两个 Buffer，每个都是初始化为 1KB，如果有百万连接，则每个连接基本占用内存大小为 1KB * 2 * 1M = 2GB。但是如果 Buffer 太小，又会导致每次 epoll 监听读事件、分发、回调处理的时候，不能够一次性把数据从内核缓冲区读出来，导致内核缓冲区还有数据，则该 fd 还会被 epoll 触发（水平触发情况下），从而导致 epoll 被多次触发影响性能。
>
> 我们使用 readv 的优势是，既不占用内存，每次读取的时候还会有一个临时栈上的缓冲区 16KB，可以保证一次读取较多的数据避免数据未读完后续 epoll 继续被触发，从而保证性能。

```
• 这题思路很好，尤其你把“初始 Buffer 不能太大”和“水平触发下要尽量读干净”联系起来了。两个细节要修正：

  1. 栈上临时缓冲区不是 16KB，当前代码是 65536，也就是 64KB。
  2. readv 不是保证“一次读完内核缓冲区”，只能提高一次系统调用承接大数据的能力。非阻塞读仍可能读不完；当前
     实现是一轮事件里读一次，不是 while 循环读到 EAGAIN。

  更标准回答：

  > Buffer 初始只有 1KB，每个连接有读写两个 Buffer，这样大量连接时基础内存占用可控。但如果每次读之前都为了
  > 可能的大包提前扩容，会浪费内存；如果只用普通 read 读到当前 writable 区，遇到突发大包又可能一次只能读很
  > 少，导致后续重复触发读事件。

  > 所以 read_from_fd() 用 readv 提供两个 iovec：第一个指向 Buffer 当前 writable 区，第二个指向栈上的 64KB
  > extraBuf。如果数据量小，就直接进 Buffer；如果超过 Buffer 剩余空间，溢出的部分先进入栈缓冲，再追加回
  > Buffer。这样用一次系统调用覆盖“写入已有空间 + 临时承接溢出数据”两个动作，减少预扩容和额外 read 的开
  > 销。
```



## TimerQueue 为什么用 timerfd？相比单独起一个定时器线程，或者用信号，有什么好处？

> [!tip]
>
> 使用 timerfd 的一个很大的好处是可以将其统一接入到事件循环里。我们可以将其注入 epoll，利用 fd 的抽象 Channel 进行事件分发和回调，这样就把网络 IO 事件和定时器事件都统一为了可以在事件循环中处理的事件，非常方便。如果另起一个线程，首先有性能开销问题，其次编程复杂度也提高了。

```
• 这题回答过关，但还可以更硬一点，把“线程安全”和“信号限制”说出来：

  > timerfd 的关键价值是把时间事件变成 fd 可读事件，所以可以像 socket、eventfd 一样注册到 epoll，由 Channel 分发回调。这样定时器回调天然在所属 EventLoop 线程执行，和连接读写、连接关闭处于同一个线程上下文，不需要
  > 额外加锁。

  补充两点：

  - 独立定时器线程的问题不是只有线程开销，更主要是回调要跨线程同步：定时器线程不能直接改某个 IO loop 的连接状态，最终还是要投递回 loop。
  - 信号方案的问题是信号处理函数限制多，只能安全调用 async-signal-safe 函数，多线程下也容易引入竞态，不适合复杂 C++ 回调逻辑。
```



## TimerQueue 里面为什么需要两个索引？expireHeap_ 和 timersById_ 分别解决什么问题？取消定时器时为什么用懒删除？

> [!tip]
>
> 我们使用一个 hashMap 作为存储所有定时器的结构，但是因为定时器触发是需要按时间排序的，所以我们又设置了一个小顶堆作为按照到期时间排序的定时器索引结构，但是注意，这个小顶堆并不是定时器的存储结构。当 epoll 中的事件循环收集到了 timerfd 上有读事件时，会分发、执行回调。我们会在小顶堆中将所有到期的定时器取出并执行定时任务。
>
> C++小顶堆使用 priority_queue 实现，不支持随机删除，因此如果某个定时器取消了我们无法精准在小顶堆中删除其，采取的方案是懒删除。我们只需要在定时器的实际存储结构即哈希表中删除定时器，当我们小顶堆 pop 时如果这个定时器已经不在哈希表中了，我们就知道这个定时器是被取消了，此时同时也将其从小顶堆中删除了。这是一种堆的懒删除策略。

```
• 这题你讲出了主干，但有一个实现细节错了：当前 timersById_ 不是 hashMap，是 std::map<TimerId, shared_ptr<Timer>>。你可以泛称“按 ID 查找的索引”，但如果说具体结构，要说 map。

  更准确版本：

  > TimerQueue 有两个索引。expireHeap_ 是 priority_queue<TimerKey, ..., greater<>>，按到期时间排序，用来快速找到最早到期的定时器，并决定 timerfd 下一次武装时间。timersById_ 是 std::map<TimerId,
  > shared_ptr<Timer>>，按 ID 管理定时器，是 Timer 对象的实际持有者，用来支持按 ID 取消和判断定时器是否仍有效。

  > 取消时只从 timersById_ 删除，因为 priority_queue 不支持高效随机删除。堆里的旧条目等到变成堆顶时再检查：如果 ID 在 timersById_ 中找不到，就说明已经取消，直接 pop 跳过；如果还存在并且到期，才执行回调。这就是懒
  > 删除。

  再给你加一个面试加分点：

  > 回调执行后还要再次检查 timersById_，因为定时器回调里可能取消自己。如果已经被删，就不能再重插周期定时器。
```



## Acceptor 里为什么要有 idleFd_？fd 耗尽时如果直接 return，会发生什么？

> [!tip]
>
> 当系统已使用 fd 到达最大限制，就会导致一个问题，TCP 三次握手后的连接即目前在全连接队列中的连接无法被消耗，这会导致 listenfd 持续有可读事件发生也就是持续被 epoll 收集，但是又无法 accept 消耗导致 cpu 空转损失性能。
>
> 当我们 accept 时，如果失败的报错信息是 fd 到达上限，此时不能直接 return，否则就会出现前面说的问题，全连接队列无法被消耗，listenfd 持续有可读事件发生也就是持续被 epoll 收集，但是又无法 accept 消耗导致 cpu 空转损失性能。
>
> 我们的解决方案是 Acceptor 中预留一个占位 fd，当发现 fd 满时，我们就关闭这个 fd，此时系统已使用 fd 从到达上限变为未到达上限，此时我们通过 accept 从全连接队列中取出、建立连接，然后再将占位 fd 赋值为取出来的这个 fd。这样就使得我们一直通过一个占位 fd 消耗全连接队列，避免前面的情况发生。

```
• 这题答得不错，而且你按当前代码讲了，不是按经典版本硬背。可以再补两个点让它更严谨：

  1. fd 耗尽可能是进程级 EMFILE，也可能是系统级 ENFILE，代码两个都处理。
  2. idleFd_ 当前打开的是 /dev/null。耗尽时关闭它腾出一个 fd 名额，然后 accept 一个挂起连接，并把这个连接 fd 接管成新的 idleFd_。这个连接不会进入业务层，最终会被作为占位 fd 持有，后续被替换/关闭。

  你的表达里“建立连接”略容易误导。准确说：三次握手已经完成了，accept 只是从 accept 队列取出这个连接 fd。不要说 accept 才建立 TCP 连接。

  可以这样答：

  > 如果 fd 耗尽时直接 return，accept 队列中的连接没有被取走，listen fd 在水平触发 epoll 下会一直可读，Acceptor::on_read() 会被反复调用但每次都 accept 失败，形成 busy-loop。idleFd_ 的作用就是给进程预留一个 fd 名
  > 额。出现 EMFILE/ENFILE 时先关闭 idle fd，腾出一个名额，再 accept 一个挂起连接，把它从 accept 队列拿走，当前实现把这个 fd 接管为新的 idle fd，从而让 accept 队列向前推进，避免 listen fd 持续可读导致 CPU 空转。
```



##  HttpContext 为什么要用 llhttp？它是怎么处理“HTTP 请求被多次 TCP 包分片送达”的？

> [!tip]
>
> 我们选型了 picohttp、httppraser、llhttp 等，最终确定了使用 llhttp。picohttp 非常简洁，似乎只有两个文件，但是缺点也是过于简洁，功能、社区不够全；httppraser 是 node.js 之前的 http 解析库，但是现在已经归档了，仓库不再更新；llhttp 是目前 node.js 使用的 http 解析库，文档社区都比较丰富，而且性能相比前一代也是提升了很多。综上，选择了性能非常强劲、文档社区丰富的 llhttp。而且经过实际 benchmark，使用了该 http 解析器后，我的整体 QPS 几乎没有损失。
>
> llhttp 会记忆解析状态，如果 buffer 中的数据不是一个完整的 http 请求，llhttp 也会解析，同时记忆当前解析的状态，比如请求行已经解析完毕，headers 正在解析等，当完整的 http 数据到来后，llhttp 解析到了完整的 http 请求后，is_complete 函数为 true，我们就可以处理这个请求了。

```
• 这题选型部分说得可以，但实现部分有两个地方要改：

  1. 不要说 is_complete 函数为 true。当前项目里没有这么对外用。HttpContext::parse() 返回的是三态：NeedMoreData、Complete、Rejected。完整性由内部 messageComplete_ 标志表示，它在 llhttp 的 on_message_complete 回调
     里置为 true。

  2. 不要说“TCP 包”。面试里最好说 TCP 字节流分片到达。HTTP 请求可能被多次 read 才读完整，不能假设一次 on_read() 就是一条完整 HTTP 请求。

  更准确回答：

  > HttpContext 每个连接一个，内部持有 llhttp parser 和当前正在构建的 HttpRequest。TCP 层每次读到一段字节后，HttpServer::on_message() 调 conn->receive() 取出本次读到的数据，然后调用 HttpContext::parse(data,
  > len)。如果这段数据还不足以构成完整 HTTP 请求，llhttp 会保留状态，parse() 返回 NeedMoreData，连接继续等下一次读事件。

  > 当后续字节继续到来时，还是喂给同一个 HttpContext，llhttp 从之前的状态继续解析。URL、header field、header value、body 都可能被分多次回调，所以项目里对应回调是 append 语义，比如 currentUrl_ 追加 URL 片段，
  > header field/value 分片时先缓存，等 value 完整后再 flush_pending_header() 写入 HttpRequest。直到 on_message_complete 被调用，messageComplete_ = true，parse() 返回 Complete，然后 HttpServer 才走
  > Router::dispatch()。

  你选型部分可以保留，但压短一点：

  > 我没有自己手写 parser，因为 HTTP 边界场景很多，包括分片输入、header 分片、body、错误请求和版本处理。llhttp 是成熟状态机解析库，性能和维护状态都比旧的 http-parser 更合适。Tudou 只负责把它的 C 回调收敛成 C++ 的
  > HttpRequest。

  还要注意一个边界：不要夸大说完整支持 HTTP pipeline。当前实现更适合一条请求解析完成后 reset context 的普通请求-响应流程；如果一个 TCP 批次里连续塞多条请求，现有流程未必完整处理队列化语义。
```

```
等一下，前一个问题我可以理解为拆包吗？你说“当前实现更适合一条请求解析完成后 reset context 的普通请求-响应流程；如果一个 TCP 批次里连续塞多条请求，现有流程未必完整处理队列化语义。”这我可以理解为粘包吗？


• 可以，但要小心措辞。

  在面试里你可以用“拆包/粘包”帮助解释 TCP 字节流问题，但最好马上补一句：TCP 本身没有消息边界，所谓拆包/粘包是应用层看到的字节流分段现象，不是 TCP 协议真的按包交付。

  对应到 HTTP：

  拆包：
  一个 HTTP 请求被多次 read 才读完整。

  read1: "GET /api HT"
  read2: "TP/1.1\r\nHost: ..."
  read3: "\r\n\r\n"

  这就是你前面问的场景。HttpContext + llhttp 可以处理，因为 parser 保留状态，parse() 返回 NeedMoreData，等后续数据继续喂进去。

  粘包：
  一次 read 读到了多个 HTTP 请求，或者一个完整请求后面还带着下一条请求的前半段。

  read1: "GET /a HTTP/1.1\r\n...\r\n\r\nGET /b HTTP/1.1\r\n...\r\n\r\n"

  这里就涉及 HTTP/1.1 pipeline 或同一连接连续请求的处理。当前项目在 HttpServer::reply_complete_request() 后会 state.httpContext.reset()，但没有显式把“本次 parse 中已经读到但属于下一条请求的剩余字节”保存下来重新喂
  给下一轮解析。所以我建议你不要宣称完整支持 pipeline/粘包队列处理。

  你可以这样回答面试官：

  > 如果说拆包，也就是一个 HTTP 请求分多次读到，Tudou 是支持的，因为 HttpContext 持有 llhttp 状态，NeedMoreData 后继续等待下一次输入。
  > 但如果说粘包，也就是一次 read 中包含多个连续 HTTP 请求，当前实现没有完整做 pipeline 队列化处理和剩余字节回放，所以我不会把它宣传成完整 HTTP/1.1 pipeline 支持。普通请求-响应和分片到达是可以处理的。

  这个回答很稳，因为你主动讲清了能力边界。
```

```
 那如果面试官问我，你想一想该如何解决粘包问题呢？


• 可以这样答：核心是不要把一次 conn->receive() 取出的字节默认等同于一条 HTTP 请求，而是给每个连接维护一个应用层输入缓冲，并让 HTTP parser 告诉你“本次消费了多少字节”。

  方案：

  1. 每个连接的 ConnectionState 里增加 pendingInput，保存尚未被完整处理的明文字节。
  2. 每次 TCP 收到数据后，把新数据 append 到 pendingInput。
  3. 用 llhttp 解析 pendingInput，解析到一条完整请求后，记录 llhttp 实际消费字节数。
  4. 从 pendingInput 删除已消费部分。
  5. 如果 pendingInput 里还有数据，继续循环解析下一条请求。
  6. 如果 parser 返回 NeedMoreData，保留剩余数据，等下一次读事件继续 append。
  7. 如果 parser 返回错误，返回 400 或关闭连接。

  口述版：

  > 我会把 HTTP 层改成每连接维护一个 pending buffer。每次从 TCP 层读到数据后，不直接假设它是一条完整请求，而是追加到 pending buffer。然后循环调用 llhttp 解析：每解析出一条完整请求，就根据 parser 消费的字节数把这
  > 部分从 pending buffer 移除，dispatch 一次 router 并发送响应。如果 buffer 里还有下一条请求的数据，就继续解析；如果数据不完整，就保留在 pending buffer 等下一次读事件。这样拆包和粘包都能统一处理。

  关键点是消费字节数。llhttp 可以通过 llhttp_get_error_pos(parser) 或回调累计偏移来知道当前处理到了哪里；更工程化的做法是在 HttpContext::parse() 返回结果里带上 consumedBytes。

  你可以设计成：

  struct ParseOutcome {
      ParseResult result;
      size_t consumedBytes;
  };

  然后 HTTP 层大概是：

  state.pendingInput.append(newData);

  while (!state.pendingInput.empty()) {
      auto outcome = state.httpContext.parse(
          state.pendingInput.data(),
          state.pendingInput.size()
      );

      if (outcome.result == NeedMoreData) {
          break;
      }

      if (outcome.result == Rejected) {
          send400();
          state.httpContext.reset();
          state.pendingInput.clear();
          break;
      }

      auto response = build_http_response(state.httpContext.get_request());
      send_http_response(conn, state, response);

      state.pendingInput.erase(0, outcome.consumedBytes);
      state.httpContext.reset();
  }

  再补一个成熟点的边界：

  > 如果要支持 pipeline，还要考虑响应顺序。HTTP/1.1 pipeline 要求响应顺序和请求顺序一致。当前业务 handler 是同步执行的，所以问题不大；如果未来 handler 异步化，就需要给每个连接维护请求序号和响应队列，不能让后完成
  > 的响应先写回。
```

```
我有一个疑问是parse() 返回 NeedMoreData时，接收到的数据它都处理了吗？


• 是的，在当前这个 HttpContext::parse() 语义下，返回 NeedMoreData 时，本次传进去的数据已经被 llhttp 消费并进入解析状态了，只是还没凑成一条完整 HTTP 请求。

  也就是说：

  read1: "GET /api HT"
  parse(read1) -> NeedMoreData

  这段 "GET /api HT" 不应该下次再喂一遍。llhttp 已经记住了解析进度，HttpContext 也已经累计了 URL、header、body 等片段。下次只需要喂新到的数据：

  read2: "TP/1.1\r\nHost: ...\r\n\r\n"
  parse(read2) -> Complete

  所以“拆包”场景下，当前实现是合理的：conn->receive() 把 TCP Buffer 清掉，HttpContext 保存协议解析状态，不需要保存原始字节。

  但“粘包/pipeline”要额外处理，因为一次输入里可能包含：

  完整请求 A + 请求 B 的一部分

  这时就不能只用现在的 NeedMoreData/Complete 三态，最好让 parse() 返回“消费了多少字节”。更稳的做法是在解析到一条完整请求时让 llhttp pause，先返回给上层处理这一条请求，然后保留未消费的剩余字节，下次继续解析。否则
  llhttp 可能继续往后解析下一条请求，导致上层不好区分请求边界。


› 我的意识是假如传入的字节流含有 content-t 就是缺失了 ype，这些字节也会被 llhttp 记住解析进度吗


• 是的，会记住，而且分两层记住：

  1. llhttp 自己记住解析状态

     比如已经进入“正在解析 header field”的状态，读到了 Content-T 但还没遇到 :，它知道这还不是完整 header。

  2. 你的 HttpContext 也会保存片段

     代码里 on_header_field_impl() 是 append：

  currentHeaderField_.append(at, length);

  所以如果第一次传入：

  Content-T

  llhttp 触发 header field 回调，currentHeaderField_ = "Content-T"。

  下一次传入：

  ype: text/plain\r\n

  llhttp 会继续解析 header field，回调里追加 ype，于是：

  currentHeaderField_ = "Content-Type"

  然后进入 header value 回调，currentHeaderValue_ 追加 text/plain。等遇到下一个 header 或 message complete 时，flush_pending_header() 才真正写入 HttpRequest。

  所以拆成这样是没问题的：

  read1: "GET / HTTP/1.1\r\nContent-T"
  parse(read1) -> NeedMoreData

  read2: "ype: text/plain\r\n\r\n"
  parse(read2) -> Complete

  注意：read1 已经被消费了，下次只喂 read2，不要把 read1 + read2 再整体喂一遍。
```



## Router::dispatch() 为什么匹配顺序是：精确路由 -> 405 -> 前缀路由 -> 404？为什么不是先做前缀匹配？

  请你按这个结构回答：

  1. 精确路由解决什么问题？
  2. 405 为什么要在前缀路由之前？
  3. 前缀路由适合什么场景？
  4. 如果所有都不匹配，为什么才是 404？

> [!tip]
>
> 首先来了一个请求，我们应该首先确定是否有业务逻辑精准匹配请求的 method + URL，因此需要先判定精确路由，保证请求能够精准匹配到业务逻辑。
>
> 然后后面的顺序其实是可以自定义的，把 405 放在 前缀路由前面是因为我们服务器的规则认为 URL 匹配但是方法不匹配时可能时由于前缀路由，我们需要给出 405 response。前缀路由主要是为了兜底使用的，一般用于静态资源分发等。404 代表 method + url 不匹配所以放在最后

```
• 整体方向对，但 405 那句有点混乱。不是“可能由于前缀路由所以给 405”，而是：

  只要精确路由表里存在同 path 的其他 method，就说明这个资源路径是存在的，只是当前 method 不允许。这个语义应该优先于前缀兜底。

  举例：

  已注册：POST /users
  已注册前缀：/users
  请求：GET /users

  如果先走前缀路由，GET /users 可能被静态兜底或通用 handler 吞掉。但 HTTP 语义上 /users 这个精确资源存在，只是不支持 GET，所以应该返回 405 Method Not Allowed，并通过 Allow header 告诉客户端允许的方法，比如 POST。

  更好的回答：

  > dispatch() 先查 method + path 精确路由，因为这是最具体的业务入口。精确没命中后，再查 allowedMethodsByPath_。如果同一个 path 注册过其他 method，说明路径存在，只是方法不允许，这时应该返回 405，并设置 Allow。这
  > 个判断要放在前缀路由之前，否则前缀兜底会吞掉本该返回 405 的请求。前缀路由更适合静态资源、统一 API 入口或目录型兜底。最后都不命中，才说明框架不知道这个资源，返回 404。

  你最后一句也要改：404 不是 “method + url 不匹配”，因为 method 不匹配但 path 存在时是 405。404 是路径/资源整体不存在。
```



## HTTPS 是怎么接入到原来的 TCP/HTTP 流程里的？为什么说 TCP 层仍然只处理字节流，HTTP 层仍然处理明文？

> [!tip]
>
> 通常来讲，HTTPS 是 HTTP + TLS，其中 TLS 的位置位于 TCP 和 HTTP 层之间，负责加密解密。TCP 仍然只处理字节流，因为它不管传递过来的是加密还是解密的内容，它只管传递字节流；HTTP 层仍然处理明文，如果 HTTP 需要发送内容，则其本质上还是发送明文，只不过到达下层 TCP 前会由 TLS 加密，然后交由 TCP 传输密文；如果 HTTP 需要接收内容，则其本质上还是接收明文，TCP 接收到的密文会由 TLS 解密后再到达 HTTP 层。因此 TLS 和 TCP、HTTP 解耦合，TCP 和 HTTP 不受影响

```
• 这个回答概念上是对的，但还缺项目实现细节。面试官会继续问：你代码里 TLS 状态放在哪里？什么时候创建？收到密文后哪一层解密？

  你要把回答落到 Tudou：

  > Tudou 里 HttpServer::enable_ssl(cert, key) 会初始化 SslContext。每条 TCP 连接建立时，HttpServer::on_connect() 会创建一个 ConnectionState，如果启用了 SSL，就为这条连接创建一个 TlsConnection。所以 TLS 状态是每
  > 连接独立的，因为每个连接都有自己的握手状态、读写 BIO 状态和加解密上下文。

  接收路径：

  > TCP 层还是只通过 TcpConnection::on_read() 和 Buffer 读字节，不理解这些字节是不是 TLS 密文。到 HttpServer::on_message() 后，如果当前连接有 TlsConnection，就先调用 read_plaintext()，把 TCP 收到的密文转换成明文
  > HTTP payload；如果 TLS 握手还没完成，可能只产生需要回写的握手密文，不会继续解析 HTTP。

  发送路径：

  > HTTP handler 生成的是明文 HttpResponse。发送前如果连接启用了 TLS，就通过 TlsConnection::write_plaintext() 加密成密文，再调用 TcpConnection::send() 发送。这样 TCP 层一直传字节，HTTP 层一直面对明文，TLS 作为二
  > 者之间的连接状态存在。
```



## connectionStates_ 为什么需要 mutex？前面你不是说 one loop per thread 可以无锁吗？

> [!tip]
>
> HTTP 层无法像 TCPServer 那样根据 EventLoop* 设置双层哈希，因此多线程访问同一个数据结构时需要加锁保证安全。

```
• 这个回答太短，方向对但说服力不够。你需要讲清楚谁会访问它、为什么不是单线程访问、为什么 TCP 层能无锁而 HTTP 层没有分片。

  更好的说法：

  > HttpServer 里 connectionStates_ 是一个全局表，key 是 TcpConnection*，value 是每条连接的 HTTP 解析状态和可选 TLS 状态。连接建立、收到消息、连接关闭都可能发生在不同 IO loop 线程中，因为 TcpServer 会把不同连接分
  > 配到不同 EventLoop。这样多个 IO 线程可能同时调用 HttpServer::on_connect()、on_message()、on_close()，共同访问同一个 connectionStates_，所以需要 contextsMutex_。

  然后补一个对比：

  > TCP 层的连接表按 EventLoop* 分片，每个内层表只由所属 loop 线程访问，所以可以无锁。HTTP 层当前没有做同样的分片，而是一个全局连接状态表，因此需要锁。这个不是和 one loop per thread 矛盾，而是数据结构的归属边界不
  > 同。

  再加一个实现细节：

  > 查找时 find_connection_state() 会在锁内找到 shared_ptr<ConnectionState> 并返回副本。拿到 shared_ptr 后就可以在锁外使用状态对象，避免长时间持有 mutex。

  不过这里要注意一个潜在追问：如果同一个连接的状态对象拿到锁外使用，是否还有并发风险？

  你的回答可以是：

  > 同一条连接的 on_message() 和 on_close() 理论上都在该连接所属 IO loop 内顺序执行，所以状态对象本身主要由单线程使用；mutex 主要保护全局 map 的增删查。不同连接对应不同 ConnectionState，可以并发处理。
```



## 你 README 里有性能测试。这个 benchmark 能证明什么？不能证明什么？如果面试官质疑“本地 hello benchmark 意义不大”，你怎么回答？

> [!tip]
>
> 这个 benchmark 能证明我们的服务器的 QPS 很高和平均响应延迟非常低，因为是本地测试，所以不能证明实际部署性能也能达到，这和服务器硬件、内存、带宽、网络也有很大的关系。我认为本地 hello benchmark 也是有一定的意义的，至少证明了我们的服务器在理想条件下能够支持这样的性能，而且我们是客户端、服务端都在本机，如果本机只做服务端，客户端放在一台局域网设备上，性能还会更高，估计百万 QPS 以上。
>
> 至于实际部署并测试 benchmark，主要是难以有这种条件，租的服务器一般带宽、性能都非常低，难以测试。

```
• 这个回答有诚实的一面，但有两个危险点：

  1. 不要说“能证明服务器 QPS 很高”这么绝对。
     更准确是：证明在特定环境、特定 workload 下，Reactor 主链路和 HTTP hello 响应路径没有明显性能瓶颈。

  2. 不要说“客户端放局域网设备上性能还会更高，估计百万 QPS 以上”。
     这是未经验证的推测，面试官很容易追问你网卡、带宽、RTT、wrk 客户端瓶颈、CPU 亲和性。你可以说“可能需要拆分客户端和服务端以排除本机 wrk 与服务端抢 CPU 的影响”，但不能直接估计更高。

  更稳的回答：

  > 这个 benchmark 主要证明 Tudou 在本机 loopback、hello response、短时间 wrk 压测这种理想场景下，主干链路性能是可接受的，并且和 muduo 处于同一数量级。它能说明 EventLoop、TcpConnection、Buffer、HTTP 解析和路由分
  > 发这些基础路径没有明显拖垮吞吐。

  然后主动说明不能证明什么：

  > 但它不能证明生产环境性能。因为测试是 localhost，没有真实网络 RTT、丢包、网卡中断、带宽限制；业务也只是内存 hello 响应，没有磁盘 IO、数据库、TLS、大文件、慢客户端、长连接空闲等复杂场景。所以我不会把它包装成生
  > 产 QPS，只把它作为框架主链路的基准测试。

  如果被质疑“本地 hello benchmark 意义不大”，你可以回答：

  > 我同意它不是完整性能评估，但它有两个价值：第一，它能做框架内部优化的相对对比，比如改 Buffer、连接表、事件分发后吞吐和延迟是否退化；第二，它能和 muduo 的 hello server 在相同机器、相同 wrk 参数下做横向数量级比
  > 较。后续如果要更严谨，我会补充跨机器压测、TLS 压测、静态文件 sendfile 压测、慢客户端场景和长连接空闲连接数压测。
```



## 下一题，项目复盘：如果面试官说：“你这个项目是不是就是仿 muduo？创新点在哪里？”你怎么回答？

> [!tip]
>
> muduo 是一个非常优秀的开源网络库，我浏览过它的源码，并且也看过陈硕写的书籍，我认为它的抽象、封装非常好和优秀。因此我在开发我的网络库的时候在类的抽象、封装上也的确参考了陈硕的设计，但是我不认为这是不足之处，陈硕的设计非常优秀，参考他的设计能让我们学习到更多。虽然参考了陈硕的设计，但是我们的网络库是从 0 开发的，代码不是直接 clone 开源代码然后修改的，我认为这有助于我更加深入理解 linux 网络编程。
>
> 至于创新点，我举一个性能上的例子。陈硕的创建、销毁连接都是由 main loop 这一个线程负责的，通过这种方式他实现了对整个 tcp connections 管理结构的无锁编程。但是每个连接的 close 回调显然是在其所属线程里面被触发的，因此为了实现这一无锁编程，他通过 queue_in_loop 这一方法将删除连接的操作投递到 main_loop，而不是由 IO loop 自己执行关闭回调。但是这可能带来性能损失，因为其他所有线程都需要向这一个 main loop 投递关闭回调，可能导致 main loop 压力大。
>
> 我则是直接使用前面讲的，设计了无锁数据结构直接实现无锁编程，无需跨线程投递关闭回调的任务，性能应该更好。

```
• 这个回答有说服力，尤其你承认参考 muduo，而不是硬说“完全原创”。但最后一句“性能应该更好”要收住，除非你有专门对比“连接关闭高并发场景”的 benchmark。否则容易被追问倒。

  你可以改成更稳的版本：

  > Tudou 的架构确实参考了 muduo，这是我主动学习经典网络库设计的结果。但我不是 clone 后改名，而是自己从 socket、epoll、eventfd、timerfd、Channel 生命周期、TcpConnection 管理、HTTP 层一路实现下来。这个过程让我真正
  > 理解了 Reactor 网络库中每个抽象为什么存在。

  然后讲差异：

  > 一个差异是连接管理。muduo 的连接表集中在 TcpServer 所在线程，连接销毁需要从 IO loop 投递回 base loop 处理。我这里做了按 EventLoop* 分片的连接记录表，外层 map 启动后只读，内层表只由所属 IO loop 修改，所以连接
  > 创建和删除可以在所属 loop 内闭环完成，减少跨线程投递。

  注意最后这样说：

  > 这个设计的优势是线程归属更直接，连接生命周期和所属 loop 绑定更紧；是否在所有场景下比 muduo 更快，需要针对连接建立/关闭压力做专项 benchmark，不能只凭直觉下结论。

  再补两个创新/工程点，避免只讲一个：

  > 另外我还做了 HTTP/HTTPS、Router、ConnectionHeartbeat、idle fd 防 fd 耗尽 busy-loop，以及 TimerQueue 的 timerfd + 最小堆 + 按 ID 索引设计。这些都是我结合项目需求做的扩展。
```



## 你刚才说连接表分片能减少跨线程删除。那这种设计有没有代价？有没有什么场景下 muduo 那种集中管理反而更简单？

> [!tip]
>
> 如果出现需要动态扩缩 TcpServer 的线程池大小的时候，我们就需要更改外层的一级哈希，我们的分片无锁设计依赖于外层哈希不更改，因此可能会比较麻烦。此时 muduo 那种集中管理反而更简单

```
• 对，这是一个很好的代价点。可以再扩展两个：

  1. 全局查询/统计更麻烦。
     比如要遍历所有连接、广播消息、统计每个连接状态，就要遍历所有 loop 的分片表，还要注意这些内层表只能在所属 loop 线程访问。集中式连接表在这类操作上更直接。

  2. shutdown 逻辑更复杂。
     你现在需要向每个 IO loop 投递清理任务，再用 activeConnectionCount_ 和条件变量等所有连接收口。集中式管理可能更容易统一删除和统计。

  你可以这样答：

  > 代价是管理复杂度会上升。这个设计依赖外层 connectionRecordsByLoop_ 在运行期不变，所以如果要动态扩缩 IO 线程，就需要重建分片或迁移连接，这会破坏当前无锁前提。其次，全局操作会更复杂，比如广播、统计所有连接、统一
  > shutdown，都需要跨多个 loop 分片协调。muduo 那种集中管理在全局可见性和生命周期统一收口上更简单，只是可能增加跨线程投递。
```



## 你在这个项目里最难的 bug 或最难的设计点是什么？请不要泛泛说“生命周期复杂”，要讲一个具体场景、为什么难、最后怎么解决。

> [!tip]
>
> 比较难的是类的抽象和设计要确保职责分离，以及随之而来的归属权、生命周期的管理。举个例子，Channel 类作为底层事件分发和触发回调的类，其归属于上层的 Acceptor 或者 TcpConnection，上层的 TcpConnection 负责 Channel 的生命周期管理。但是如果 Channel 触发的回调是关闭连接，那么可能导致 TcpConnection 被销毁，连带着自己也会被销毁，但是自己还正在回调的函数栈帧里，这会导致内存访问错误。因此使用的是 Channel 内部持有一个 weak_ptr 观察 TcpConnection，在 Channel 触发回调时，会临时将 weak_ptr 升级为 shared_ptr，确保执行回调期间 TcpConnection 引用计数不归零，临时延长其生命周期，确保生命周期管理正确、内存访问安全。

```
• 这个回答是合格的，而且是你目前最稳的一类问题。再打磨一下会更强：你已经讲了 tie，但还可以补上 fd/epoll 注销顺序，这样从“对象活着”扩展到“内核事件也安全”。

  更强版本：

  > 最难的一个具体点是 TcpConnection、Channel 和 epoll 之间的生命周期。Channel 是底层事件分发对象，但它归属于 TcpConnection。当 epoll 返回连接 fd 的事件后，执行栈进入 Channel::handle_events()，再触发
  > TcpConnection::on_read/on_close。如果用户回调里关闭连接，TcpServer 从连接表删除它，最后一个 shared_ptr 可能释放，TcpConnection 析构，内部 Channel 也析构。但此时栈还在 Channel::handle_events()，这就有 use-
  > after-free 风险。

  > 解决上我用了两层保护。第一是 Channel::tie_to_object()：Channel 保存 weak_ptr，事件分发入口临时 lock() 成 shared_ptr，保证本轮回调期间 TcpConnection 不析构。第二是析构顺序：Channel 析构时只从 epoll 注销，不
  > close fd；Socket 才负责 close fd。TcpConnection 中 connSocket_ 声明在 channel_ 前面，让 channel_ 先析构注销 epoll，随后 Socket 再 close fd，避免 epoll 持有悬空的 Channel* 或已关闭 fd 状态。
```



##  如果让你支持百万长连接，你觉得当前 Tudou 哪些地方会成为瓶颈？你会怎么优化？

> [!tip]
>
> 我可能会想到从 CPU、内存上进行优化。
>
> 首先是 CPU，百万并发肯定要使用多核 CPU，并且尽量榨干每一个核心的性能。因此我们的多线程应该在工作量上应该平衡，我们目前采用的是 RR 轮询算法将接收到的连接分配到各个线程的 eventloop，可以考虑更加先进的算法如最少连接数等其他负载均衡算法。
>
> 内存的话，每个连接的 buffer 不能太大也不能太小，我们目前设计的 gather io（readv）我感觉是还可以的，兼顾了内存和性能。可以优化的地方可能在细节上，避免不必要的拷贝，例如通过移动代替拷贝等。

```
• 方向对，但回答偏浅。百万长连接面试官期待你不只说 CPU/内存，还要说 fd、内核参数、accept 瓶颈、定时器、慢连接、业务负载边界。

  你可以这样补强：

  > 百万长连接首先不是单纯 QPS 问题，而是连接数、内存、fd 和调度问题。系统层面要调高 ulimit -n、fs.file-max、somaxconn 等参数，否则 fd 和 listen backlog 会先成为瓶颈。

  CPU 方面：

  > 当前新连接是 round-robin 分配到 IO loop，这在连接负载均匀时可以，但长连接场景下每条连接活跃度不同，round-robin 不一定均衡。可以改成按每个 loop 的连接数、活跃事件数或 pending task 队列长度做负载分配。accept 也
  > 可能成为单 main loop 瓶颈，可以考虑 SO_REUSEPORT 多 acceptor，让多个 loop 各自 accept。

  内存方面：

  > 每条连接至少有 TcpConnection、Channel、Socket、读写 Buffer、可能还有 ConnectionHeartbeat 和 HTTP/TLS 状态。百万连接时对象数量巨大，应该控制 Buffer 初始大小，避免每连接预分配过大内存，并考虑对象池、内存池或
  > jemalloc，减少碎片和分配开销。

  注意一个术语：

  > readv 更准确叫 scatter read，读入多个缓冲区；writev 才常被称为 gather write。你可以统称 scatter/gather I/O，但不要单独说 “readv 是 gather I/O”。

  再补一个很重要的点：

  > 如果每条连接都启用心跳定时器，百万连接就是百万级定时器。当前 TimerQueue 是堆 + ID 索引，插入取消是 O(log n)，能工作，但高频刷新和取消仍有成本。长连接空闲检测可以考虑时间轮，或者按 loop 分桶扫描，降低大量定时
  > 器的维护开销。
```

