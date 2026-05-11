# Architecture Overview

```mermaid
%%{init: {
    "theme": "base",
    "themeVariables": {
        "fontFamily": "Inter, Helvetica, sans-serif",
        "fontSize": "16px",
        "primaryColor": "#eef2f3",
        "primaryTextColor": "#333",
        "primaryBorderColor": "#6c7a89",
        "lineColor": "#6c7a89",
        "secondaryColor": "#dce8f0",
        "tertiaryColor": "#fdfdfd"
    }
}}%%

flowchart TD
    %% 样式定义
    classDef appLayer fill:#f9e79f,stroke:#f39c12,stroke-width:2px,color:#333
    classDef httpLayer fill:#f5cba7,stroke:#d35400,stroke-width:2px,color:#333
    classDef tcpLayer fill:#aed6f1,stroke:#2980b9,stroke-width:2px,color:#333
    classDef reactorLayer fill:#abebc6,stroke:#27ae60,stroke-width:2px,color:#333
    classDef osLayer fill:#d2b4de,stroke:#8e44ad,stroke-width:2px,color:#333
    classDef invisible stroke-width:0,fill:none

    subgraph AppBusiness ["应用业务层 (App Layer)"]
        direction LR
        App["用户应用\n(StaticFileHttpServer / FileLinkServer / StarMind)"]
        UserCallbacks["业务回调\nonConnection()\nonMessage()\nonRequest()"]
        App --> UserCallbacks
    end

    subgraph HttpLayer ["HTTP/TLS 协议层 (Protocol Layer)"]
        direction TB
        HttpSrv["HttpServer"]
        HttpCtx["HttpContext\n(基于 llhttp)"]
        HttpReq["HttpRequest / HttpResponse"]
        TlsConn["TlsConnection / SslContext"]
        Router["Router 路由分发"]
        
        HttpSrv --> HttpCtx
        HttpSrv --> TlsConn
        HttpCtx --> HttpReq
        HttpSrv --> Router
    end

    subgraph NetLayer ["TCP 网络层 (TCP Layer)"]
        direction TB
        TcpSrv["TcpServer"]
        Acceptor["Acceptor\n处理新连接"]
        TcpConn["TcpConnection\n连接管理"]
        Buffer["Buffer\n读写缓冲区"]
        Heartbeat["ConnectionHeartbeat\n心跳机制"]
        
        TcpSrv -->|接受新连接| Acceptor
        TcpSrv -->|创建 & 管理| TcpConn
        TcpConn -->|数据缓冲| Buffer
        TcpConn -->|超时检查| Heartbeat
    end

    subgraph ReactorLayer ["Reactor 事件分发层 (Reactor Layer)"]
        direction TB
        ThreadPool["EventLoopThreadPool\n多线程模型"]
        MainLoop["Main EventLoop\n主反应堆"]
        SubLoop["Sub EventLoop\n子反应堆"]
        Poller["EpollPoller\nI/O 多路复用"]
        Channel["Channel\n事件分发"]
        TimerQ["TimerQueue / Timer\n定时任务队列"]
        
        ThreadPool --> MainLoop
        ThreadPool -->|分配连接| SubLoop
        MainLoop --> Poller
        SubLoop --> Poller
        Poller -->|触发事件| Channel
        SubLoop --> TimerQ
    end

    subgraph OS_Kernel ["操作系统层 (OS Kernel)"]
        direction LR
        Epoll["epoll\nepoll_wait()"]
        Socket["Sockets\naccept(), read(), write()"]
        EventFd["eventfd\n跨线程唤醒"]
        TimerFd["timerfd\n定时触发"]
        
        Epoll -.-> Socket
        Epoll -.-> EventFd
        Epoll -.-> TimerFd
    end

    %% 层级间依赖与数据流
    AppBusiness ==>|注册回调 & 业务处理| HttpLayer
    HttpLayer ==>|解析封装 & 依赖底层| NetLayer
    NetLayer ==>|事件注册与分发| ReactorLayer
    ReactorLayer ==>|系统调用| OS_Kernel

    %% 赋予各个Subgraph颜色
    class AppBusiness,App,UserCallbacks appLayer
    class HttpLayer,HttpSrv,HttpCtx,HttpReq,TlsConn,Router httpLayer
    class NetLayer,TcpSrv,Acceptor,TcpConn,Buffer,Heartbeat tcpLayer
    class ReactorLayer,ThreadPool,MainLoop,SubLoop,Poller,Channel,TimerQ reactorLayer
    class OS_Kernel,Epoll,Socket,EventFd,TimerFd osLayer
```

# Architecture UML

本文档包含服务端的核心类图，分为简略版（展示系统中各类之间的拓扑关系）和详细版（展示各模块的类详细设计）。

## 1. 类关系简略图

此图展示了系统中众多组件（涵盖 HTTP、TCP 和 Reactor 核心）之间的 UML 关系，重点在于表示组件之间的依赖、拥有和生命周期关系。利用连线方向和内置样式实现从上到下的自然分层与美化，并通过模块划分使结构更加清晰。

```mermaid
%%{init: {
    "theme": "base",
    "themeVariables": {
        "fontFamily": "Inter, Helvetica, sans-serif",
        "fontSize": "16px",
        "primaryColor": "#eef2f3",
        "primaryTextColor": "#333",
        "primaryBorderColor": "#6c7a89",
        "lineColor": "#6c7a89"
    }
}}%%
classDiagram
    direction TD

    subgraph Http 模块
        class HttpServer
        class Router
        class HttpContext
        class HttpRequest
        class HttpResponse
    end

    subgraph Tcp 模块
        class TcpServer
        class TcpConnection
        class ConnectionHeartbeat
        class Acceptor
        class Buffer
        class Socket
    end

    subgraph Reactor 模块
        class EventLoopThreadPool
        class EventLoopThread
        class EventLoop
        class EPollPoller
        class TimerQueue
        class Timer
        class Channel
    end

    %% Http 层 -> Tcp 层 -> Reactor 层的连线顺序决定了渲染时的从上到下排版
    
    %% Http 内部关系
    HttpServer "1" *-- "1" Router: owns
    HttpServer "1" *-- "n" HttpContext: manages
    HttpContext "1" *-- "1" HttpRequest: owns
    HttpContext "1" *-- "1" HttpResponse: owns
    
    %% Http 与 Tcp 边界
    HttpContext --> TcpServer: callbacks

    %% Tcp 内部关系
    TcpServer "1" *-- "1" Acceptor: owns
    TcpServer "1" *-- "n" TcpConnection: owns(shared_ptr)
    ConnectionHeartbeat --> TcpConnection: monitors
    TcpConnection "1" *-- "2" Buffer: owns(read/write)
    TcpConnection "1" *-- "1" Socket: owns
    Acceptor "1" *-- "1" Socket: owns
    
    %% Tcp 与 Reactor 边界
    TcpServer "1" --> "1" EventLoop: main_loop
    TcpServer "1" *-- "1" EventLoopThreadPool: owns
    Acceptor "1" *-- "1" Channel: owns
    TcpConnection "1" *-- "1" Channel: owns

    %% Reactor 内部关系
    EventLoopThreadPool "1" *-- "n" EventLoopThread: manages
    EventLoopThread "1" *-- "1" EventLoop: creates
    EventLoop "1" *-- "1" EPollPoller: owns
    EventLoop "1" *-- "1" TimerQueue: owns
    TimerQueue "1" *-- "n" Timer: manages
    EPollPoller "1" --> "n" Channel: tracks
```

## 2. TCP / Reactor 模块核心类详细图

此图涵盖了 Reactor 模式的核心事件循环组件与 TCP 层的连接管理组件，详尽展示了各类的职责与接口。

```mermaid
classDiagram
    direction TD

    subgraph Reactor核心
        class EventLoop {
            -std::unique_ptr~Poller~ poller
            +loop()
            +update_channel(Channel* channel)
            +remove_channel(Channel* channel)
        }

        class EPollPoller {
            -int epollfd_
            -std::vector~struct epoll_event~ events_
            +poll(int timeoutMs, ChannelList* activeChannels) Timestamp
            +updateChannel(Channel*) override
            +removeChannel(Channel*) override
        }

        class Channel {
            -EventLoop* loop
            -int fd
            -int events
            -int revents
            
            -handle_read()
            +set_read_callback(std::function cb)
            -handle_write()
            +set_write_callback(std::function cb)
            -handle_close()
            +set_close_callback(std::function cb)
            -handle_error()
            +set_error_callback(std::function cb)
        }

        class EventLoopThreadPool {
            -EventLoop* mainLoop_
            -std::vector~std::unique_ptr~EventLoopThread~~ threads_
            +start()
            +get_next_loop() EventLoop*
            +get_all_loops() vector~EventLoop*~
        }

        class EventLoopThread {
            -EventLoop* loop_
            -std::thread thread_
            +start()
            +get_loop() EventLoop*
            -thread_func()
        }

        class TimerQueue {
            -EventLoop* loop_
            -TimerMap timers_
            +add_timer(callback, when, interval) TimerId
            +cancel_timer(timerId)
        }

        class Timer {
            -TimerId id_
            -Callback callback_
            -Timestamp expiration_
            -milliseconds interval_
            +run()
            +restart(now)
            +is_repeat() bool
        }
    end

    subgraph TCP网络层
        class TcpServer {
            -EventLoop* loop
            -UniquePtr acceptor
            -ConnectionMap connections
            -MessageCallback messageCallback
            
            +set_message_callback(MessageCallback cb)
        }
        
        class Acceptor {
            -EventLoop* loop
            -int listenFd
            -std::unique_ptr~Channel~ channel
            
            -read_callback()
            -handle_connect(int connFd)
            +set_connect_callback(std::function cb)
        }

        class TcpConnection {
            -EventLoop* loop
            -int connectFd
            -unique_ptr channel
            -MessageCallback messageCallback
            -CloseCallback closeCallback
            
            -read_callback()
            -write_callback()
            -close_callback()
            -handle_message()
            -handle_close()
            +set_message_callback(MessageCallback cb)
            +set_close_callback(CloseCallback cb)
        }

        class Buffer {
            -std::vector~char~ buffer
            -size_t readIndex
            -size_t writeIndex
            +read_from_buffer(size_t len) string
            +write_to_buffer(data, len)
            +read_from_fd(fd, savedErrno) ssize_t
            +write_to_fd(fd, savedErrno) ssize_t
            +readable_bytes() size_t
        }

        class Socket {
            -int sockFd_
            +fd() int
            +create_tcp_listener(addr) Socket$
        }

        class ConnectionHeartbeat {
            -EventLoop* loop_
            -std::weak_ptr~TcpConnection~ connection_
            +start()
            +refresh()
            -on_timeout()
        }
    end

    %% Reactor组合/聚合关系
    EventLoop "1" *-- "1" EPollPoller: owns
    EPollPoller "1" --> "n" Channel: tracks
    EventLoop "1" *-- "1" TimerQueue: owns
    TimerQueue "1" *-- "n" Timer: manages
    EventLoopThreadPool "1" *-- "n" EventLoopThread: manages
    EventLoopThread "1" *-- "1" EventLoop: creates

    %% TCP组合/聚合关系
    Acceptor "1" *-- "1" Channel: owns
    Acceptor "1" *-- "1" Socket: owns
    TcpConnection "1" *-- "1" Channel: owns
    TcpConnection "1" *-- "2" Buffer: owns
    TcpConnection "1" *-- "1" Socket: owns
    TcpServer "1" *-- "1" Acceptor: owns
    TcpServer "1" *-- "n" TcpConnection: owns(shared_ptr)
    TcpServer "1" --> "1" EventLoop: main_loop
    TcpServer "1" *-- "1" EventLoopThreadPool: owns
    ConnectionHeartbeat --> TcpConnection: monitors
```

## 3. HTTP 模块核心类详细图

此图专注于 HTTP 协议栈的解析与路由逻辑，涵盖 HttpServer 的生命周期与路由分发、TLS/SSL 加密层、HttpContext 的解析状态机、以及相关的 HTTP 请求与响应实体。

```mermaid
classDiagram
    direction TD

    class HttpServer {
        -std::unique_ptr~TcpServer~ tcpServer_
        -Router router_
        -std::unordered_map~int, ConnectionState~ connectionStates_
        -std::unique_ptr~SslContext~ sslContext_
        
        +start()
        +add_route(method, path, handler)
        +enable_ssl(certFile, keyFile)
        +process(conn)
        -bind_tcp_callbacks()
        -on_connect(conn)
        -on_close(conn)
        -parse_http_request(ctx, payload)
        -reply_complete_request(conn, state, ctx)
    }

    class HttpContext {
        -llhttp_t parser_
        -HttpRequest request_
        -bool messageComplete_
        
        +parse(data, len, nparsed) bool
        +is_complete() bool
        +get_request() HttpRequest
        +reset()
        -on_message_begin(parser) int$
        -on_url(parser, at, length) int$
        -on_header_field(parser, at, length) int$
        -on_header_value(parser, at, length) int$
        -on_body(parser, at, length) int$
        -on_message_complete(parser) int$
    }

    class Router {
        -std::unordered_map~RouteKey, Handler~ exactRoutes_
        -std::vector~PrefixRoute~ prefixRoutes_
        
        +dispatch(req, resp) DispatchResult
        +add_route(method, path, handler)
        +add_prefix_route(prefix, handler)
        -find_exact_handler(req)
        -find_prefix_handler(path)
    }

    class HttpRequest {
        -std::string method_
        -std::string path_
        -std::string query_
        -std::unordered_map~string, string~ headers_
        -std::string body_
    }

    class HttpResponse {
        -int statusCode_
        -std::string statusMessage_
        -std::unordered_map~string, string~ headers_
        -std::string body_
    }

    class SslContext {
        -SSL_CTX* ctx_
        +init(certFile, keyFile) bool
        +create_ssl() SSL*
        +is_initialized() bool
    }

    class TlsConnection {
        -SSL* ssl_
        -State state_
        +read(data, len, bytesRead) ReadResult
        +write(data, len, bytesWritten) ReadResult
        +do_handshake() ReadResult
        +is_established() bool
    }

    %% HTTP 模块内部关系
    HttpServer "1" *-- "1" Router: owns
    HttpServer "1" *-- "n" HttpContext: manages
    HttpServer "1" *-- "1" SslContext: owns
    HttpServer "1" *-- "n" TlsConnection: manages(in ConnectionState)
    HttpContext "1" *-- "1" HttpRequest: owns
    HttpContext ..> HttpResponse: parses/creates
    Router ..> HttpRequest: reads
    Router ..> HttpResponse: mutates
    TlsConnection --> SslContext: uses(SSL session)
```

# 时序图

## Reactor 模式事件触发时序图

```mermaid
%%{init: {
    "theme": "default",
    "themeVariables": {
        "fontFamily": "Times New Roman",
        "fontSize": "20px"
    }
}}%%

sequenceDiagram
    title Reactor 反应堆时序图
    autonumber

    actor one(App线程/其他线程)
    participant EventLoop
    participant Poller
    participant Channel
    participant TcpAcceptor
    participant TcpConnection

    one(App线程/其他线程) ->> EventLoop: loop()
    loop
        EventLoop->>Poller: poll(timeoutMs)

        activate Poller
        Poller->>Poller: get_activate_channels()
        deactivate Poller
        
        Poller->>EventLoop: active channels
        EventLoop->>Channel: channel→handle_events()
        Channel->> TcpAcceptor: handle_read()/callbacks...
        Channel->> TcpConnection: handle_read()、close().../callbacks...
    end
    EventLoop->>Poller: poller→poll()

```

## TCP 模块事件触发时序图

```mermaid
%%{init: {
    "theme": "default",
    "themeVariables": {
        "fontFamily": "Times New Roman",
        "fontSize": "18px"
    }
}}%%
sequenceDiagram
    autonumber
    title Tudou 网络库回调触发关系（精简版）

    participant EventLoop
    participant EpollPoller
    participant Channel
    participant Acceptor
    participant TcpConnection
    participant TcpServer
    actor App as 上层应用

    %% 新连接到来：从监听 fd 上的读事件一路触发到上层

    EventLoop ->> EpollPoller: poll()
    EpollPoller -->> EventLoop: activeChannels(listen-fd)
    EventLoop ->> Channel: handle_events() on listen-fd
    Channel ->> Acceptor: handle_read()  // readCallback

    Acceptor ->> Acceptor: accept() 得到 connFd
    Acceptor ->> TcpServer: connectCallback(connFd)
    TcpServer ->> TcpConnection: 构造连接对象并注册回调
    TcpServer ->> App: connectionCallback(conn)  // 通知上层有新连接

    %% 有数据可读：从 conn-fd 上的读事件触发到应用层 message 回调

    EventLoop ->> EpollPoller: poll()
    EpollPoller -->> EventLoop: activeChannels(conn-fd, EPOLLIN)
    EventLoop ->> Channel: handle_events() on conn-fd
    Channel ->> TcpConnection: handle_read() -> read_callback()

    TcpConnection ->> TcpConnection: readBuffer.read_from_fd()
    TcpConnection ->> TcpConnection: handle_message()
    TcpConnection ->> TcpServer: messageCallback(conn)
    TcpServer ->> App: messageCallback(conn)  // 上层处理业务 / HTTP 请求

    %% 上层发送响应：从 App 的 send 调用触发到底层写 fd

    App ->> TcpConnection: send(response)
    TcpConnection ->> TcpConnection: writeBuffer.write_to_buffer()
    TcpConnection ->> Channel: enable_writing()

    EventLoop ->> EpollPoller: poll()
    EpollPoller -->> EventLoop: activeChannels(conn-fd, EPOLLOUT)
    EventLoop ->> Channel: handle_events() on conn-fd (write)
    Channel ->> TcpConnection: handle_write() -> write_callback()
    TcpConnection ->> TcpConnection: writeBuffer.write_to_fd()

    %% 连接关闭：从底层事件一路触发到上层的关闭回调

    alt 对端关闭或出错
        EventLoop ->> EpollPoller: poll()
        EpollPoller -->> EventLoop: activeChannels(conn-fd, EPOLLIN/HUP/ERR)
        EventLoop ->> Channel: handle_events()
        Channel ->> TcpConnection: handle_close()/handle_error()
        TcpConnection ->> TcpConnection: handle_close()
        TcpConnection ->> TcpServer: closeCallback(conn)
        TcpServer ->> TcpServer: remove_connection(conn)
        TcpServer ->> App: connectionCallback(conn 关闭)  // 如果你选择在这里通知上层
    end
```