## InetAddress Refactoring Log

### Situation
旧版 InetAddress 把 IPv4 构造过程直接揉进构造函数内部，初始化步骤、字节序转换和文本地址解析杂糅在一起，而且完全忽略了 inet_pton 的返回值。这意味着非法 IP 文本会被静默吞下，调用方只能在更远处以不确定行为的形式感知问题。头文件中的注释也停留在“封装 sockaddr_in”的表述层面，没有把对象的输入契约和职责边界讲清楚。

### Task
本次破坏式重构的目标不是给旧实现继续打补丁，而是把 InetAddress 收敛为一个契约明确的 IPv4 值对象。我要把构造流程拍平成可读的线性步骤，把所有隐式约束前置到对象边界，并用单元测试把正常路径和失败路径一起锁死。

### Action
我把基于文本地址的构造拆成了 create_empty_address、assign_family、assign_port、assign_ip 四个原子步骤，由构造函数作为唯一编排入口顺序驱动，彻底消除“读一眼看不出初始化顺序”的问题。对于从原生 sockaddr_in 构造的路径，我单独前置了 ensure_ipv4_family，拒绝非 AF_INET 的伪契约输入继续扩散。与此同时，我把 get_ip、get_port、get_ip_port 背后的字节序转换与格式化逻辑下沉为纯工具函数，保持公有接口只表达业务语义，不承载底层细节。最后，我补上了非法 IPv4 文本和非法地址族的失败测试，确保契约收紧不是停留在注释层，而是有可执行的保护网。

### Result
重构后，InetAddress 不再是一个“能用就行”的薄封装，而是一个显式守住输入边界的值对象。非法地址现在会在进入系统的第一时间失败，网络字节序和文本格式化规则被统一收口，调用方不需要再猜测对象内部状态是否可靠。代码结构从隐式堆叠的构造片段，变成了可沿调用顺序直接阅读的线性流程，这让后续扩展 IPv4/IPv6 分层、日志排障和面试场景下的架构讲述都更有说服力。

## TCP Reactor Refactoring Log

### Situation
旧版 tcp 子系统里散落着几条很典型的“历史避坑注释”。EventLoop 明确警告 pending functor 绝不能拿引用，因为 pop 后会悬空；TcpServer 注释强调连接必须在目标 IO 线程里一次性装配完成，否则回调可能先于初始化触发；Acceptor 只留下了 LT 模式下一次 accept 一个连接的经验说明，但没有把“新连接一出生就必须是 non-blocking/CLOEXEC”这种运行时契约固化到代码结构里。更隐蔽的问题是 TimerQueue 在添加首个定时器后并没有稳定地同步 timerfd，导致定时器能否准时触发依赖隐式状态，而不是依赖显式流程。

### Task
这次破坏式重构的目标不是继续给这些注释打补丁，而是把 Reactor 子系统重塑成一条可线性阅读的单向流程：接入层只负责 accept 并发布连接，事件循环只负责 poll 与调度，定时器层只负责“收集到期任务、执行、重设下次唤醒”。所有旧注释里提到的坑，都要从“提醒开发者小心”升级成“代码结构本身不再允许踩坑”。

### Action
我先把 EventLoop 的 pending functors 流程拆成 take_pending_functors 和 execute_pending_functors 两个原子步骤，让“必须值拷贝”这个历史坑被固定在一个可审计的执行点里。接着把 TimerQueue 拆成 on_timerfd_read -> collect_expired_timers -> execute_expired_timers -> sync_timerfd 的单层编排，并修掉了首个定时器注册后没有可靠 arm timerfd 的根因，同时补上空队列时显式 disarm timerfd，避免过期状态残留。接入层则把 Acceptor 改为 accept4，强制新连接在进入系统的第一时间就具备 non-blocking 和 close-on-exec 契约，不再把 socket 属性正确性留给后续层补救。最后，我把 EventLoopThread 和 EventLoopThreadPool 也重排成显式的启动步骤，并补上针对首个定时器触发、跨线程 wakeup、accept4 连接属性以及线程池轮询行为的单元测试，用测试网把这些新契约锁死。

### Result
重构后，tcp Reactor 不再依赖零散注释去提醒“这里很危险”，而是把这些危险点压缩成了清晰的门面和原子步骤。首个定时器现在会稳定接管 timerfd 的唤醒时间，跨线程任务投递路径具备可验证的唤醒语义，新接受的连接从出生开始就满足非阻塞契约，线程池的 loop 选择行为也有了明确测试覆盖。过去那些只能靠经验记忆的坑，现在已经变成代码结构和单元测试共同维护的显式架构约束。

## HTTP Parser Contract Refactoring Log

### Situation
HttpContext 这一层原本已经有大量关于 llhttp 分片回调的注释，明确说明 URL、Header 和 Body 都可能被拆成多段送达。但旧实现只把 Header 和 Body 真正按分片语义处理，URL 却在 on_url 回调里每次都直接覆盖到 HttpRequest，HTTP 版本也被硬编码成 HTTP/1.1。这导致代码表面上声称自己尊重流式解析契约，实际却会在 request line 跨 chunk 时悄悄丢失路径片段，并把 HTTP/1.0 请求错误伪装成 1.1。

### Task
这次破坏式重构的目标不是继续给解析流程补边角判断，而是把 HttpContext 收敛成一个契约真实可执行的解析门面。我要让 llhttp 的分片语义在代码结构上被严格兑现，让请求对象只在拿到完整状态后才暴露稳定结果，并用回归测试锁死这一点。

### Action
我把请求目标处理重构为“累计 currentUrl_ -> message complete 时统一 apply_request_target -> 再根据 parser_.http_major/http_minor 落版本”的单向流程，彻底消除了 URL 分片时的覆盖型写入。与此同时，我保留 Header/Body 的 append 语义，把 parse() 继续作为唯一对外入口，避免调用方理解 llhttp 的内部回调细节。最后，我补上了 split request line 的单元测试，专门验证跨 chunk 的 URL、query 和 HTTP/1.0 版本不会再被吞掉或伪造。

### Result
重构后，HttpContext 不再只是“看起来像流式解析器”，而是真正兑现了分片输入契约。请求对象的 path、query 和 version 现在都来自完整且一致的解析状态，旧代码里那种只有在特定分包条件下才暴露的问题被前置并消灭掉了。对外暴露的 parse 门面保持简单，但内部数据流已经从隐式覆盖写，变成了清晰可验证的单向组装。

## HTTP TLS Facade Refactoring Log

### Situation
旧版 HTTP/TLS 路径里最典型的历史包袱，直接写在注释里了。TlsConnection 头文件和 HttpServer 实现都明确提醒过一件事：TLS 握手产生的响应数据必须立刻从 get_output() 取出并回写给客户端，否则状态机无法继续推进。问题在于，这个关键约束只是散落在注释和局部分支里的经验，而不是被收敛成一条显式流程。与此同时，HttpResponse 上的 closeConnection_ 只存在于对象字段里，却不会实际体现在序列化结果中，形成“代码写了契约，协议没执行契约”的典型漂移。

### Task
本次重构的任务是把 HTTP/HTTPS 门面重塑成一条可沿调用顺序直接阅读的流程图，让 TLS 输入、握手推进、握手输出回写、解密、业务响应发送都成为显式原子步骤。同时，我要把 Response DTO 中悬空的 closeConnection 契约补齐，避免字段和协议表现继续分裂。

### Action
我把 HttpServer::process 拆成 resolve_request_context、log_incomplete_request、reject_bad_request、reply_complete_request、send_http_response 等直接子步骤，把“解析失败”和“业务响应”两条路径都统一收口到发送门面。TLS 输入归一化也被拆成 feed_tls_ciphertext、advance_tls_handshake、flush_tls_output、decrypt_tls_payload 四个原子动作，旧注释里要求开发者记住的“必须立刻回写握手包”现在被固定成一个专门步骤，不再依赖阅读者的经验。底层的 SslContext 和 TlsConnection 则被重构成线性的初始化与错误收敛流程，补上了重复初始化清理、空 SSL 句柄防御和 Memory BIO 失败路径。最后，我为 SslContext、TlsConnection、HttpResponse 和 HttpRequest 补了单元测试，覆盖证书加载、握手收发、closeConnection 序列化以及 DTO 状态清空等关键边界。

### Result
重构后，HTTP/TLS 子系统不再靠注释提醒“这里千万别忘了发握手数据”或者“这个 close 标志理论上应该生效”。TLS 输出路径已经被架构化为显式步骤，closeConnection 契约也能稳定反映到真实报文里。调用链从过去夹杂隐式协议知识的树状分支，变成了门面驱动、步骤清晰、测试可证的扁平 DAG，这使后续继续扩展 HTTPS、连接管理或面试讲述时都具备更高的可解释性和可维护性。

## NonCopyable Contract Refactoring Log

### Situation
旧版 NonCopyable 的注释只强调“被继承以后无法拷贝构造和赋值”，但真正的类型契约并没有被完整说清楚。移动构造和移动赋值处于隐式状态，构造函数和析构函数还保留着空函数体风格，等于把“这个基类到底只是禁止 copy，还是连 move 也应该禁止”留给了阅读者猜。更关键的是，这个所有权约束完全没有测试保护，只要后续有人改动一行声明，类型语义就可能漂移。

### Task
本次重构的目标是把 NonCopyable 收敛成一个自解释的所有权 mixin，让“不可复制、不可移动”成为显式、稳定且可验证的契约，而不是停留在含混注释里的约定俗成。

### Action
我把 NonCopyable 头文件改成了明确的契约声明：显式删除 copy ctor、copy assignment、move ctor、move assignment，并把构造与析构收敛为受保护的 default 实现，去掉旧式空函数体。与此同时，我新增了与 src/base 对齐的 [test/unitTest/base/NonCopyableTest.cpp](test/unitTest/base/NonCopyableTest.cpp)，通过 type_traits 把“派生类既不可拷贝也不可移动”锁成可执行断言，彻底避免所有权语义在后续演进中悄悄变松。

### Result
重构后，NonCopyable 不再是一个靠注释提示使用者的小技巧，而是一个边界清晰的类型契约。所有权限制已经从隐式推断升级为显式声明加测试兜底，后续读代码的人不需要再猜测 move 语义是否被允许，构建系统也会在契约被破坏时第一时间发出信号。

## Router And Test Topology Refactoring Log

### Situation
Router 这块的旧注释其实已经暴露了几个很关键的历史坑：精确路由必须先于前缀路由，405 必须先于前缀兜底，前缀路由的优先级等于注册顺序。这些规则决定了分发结果，但之前的测试目录却是平铺在 test/unitTest 根目录下，结构上完全看不出它们分别守护 src/base、src/tudou/http、src/tudou/router、src/tudou/tcp 哪个子系统。更糟的是，“前缀注册顺序”这个注释级契约并没有专门测试，一旦有人重构成按最长前缀或别的排序策略，分发语义就可能被无声改写。

### Task
这次重构的任务不是继续接受“规则写在注释里，测试散在根目录里”的状态，而是把测试结构和源码结构对齐，让每个测试文件的职责边界可视化，并把 Router 最核心的优先级约束升级成可执行的回归保护网。

### Action
我先把 unit test 目录重组为与 src 对齐的镜像结构：base、tudou/http、tudou/router、tudou/tcp 四个子树分别承接各自模块的测试，再把 [test/unitTest/CMakeLists.txt](test/unitTest/CMakeLists.txt) 改成递归收集测试源文件，消除“测试只能平铺在根目录”这个构建约束。随后，我为 [test/unitTest/tudou/router/RouterTest.cpp](test/unitTest/tudou/router/RouterTest.cpp) 补上了“多个前缀都命中时，必须按注册顺序选择第一个处理器”的回归用例，并为 [test/unitTest/base/InetAddressTest.cpp](test/unitTest/base/InetAddressTest.cpp) 增加值语义拷贝赋值测试，确保 base 层和值对象层的契约也有对应保护。

### Result
重构后，测试目录本身已经成为架构的一部分，开发者只看路径就能知道某个测试在守哪一层源码。Router 那些原本只能靠阅读注释记住的分发优先级，现在已经由明确的回归测试兜底；测试和源码的关系从“平铺堆积”变成“结构映射”，这会显著降低后续继续拆分模块、补测试和做代码评审时的认知成本。