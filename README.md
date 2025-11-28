# Reactor 模式的 IO 多路复用

## Reactor 模式

![Reactor](/doc/Reactor.png)

## 系统架构图

## Reactor 模块时序图

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

## 整体时序图（Callback 回调流向）

各类及其职责说明：
- EventLoop：事件循环驱动者。持有 EpollPoller，使用 EpollPoller 获取 activeChannels 并驱动 activeChannels 触发回调处理事件
- EpollPoller：多路复用及 channels 管理中心。封装了 epoll_create、epoll_wait、epoll_ctl 等系统调用
- Channel：回调分发器。封装了 fd 及其感兴趣的事件，负责在事件发生时根据相应的事件调用相应的回调函数处理事件
- Acceptor：连接接受器。持有 fd、channel；还持有回调函数 connectCallback 用于执行上层 TcpServer 的连接回调，当建立新连接时触发调用。callback 用于处理 channel 新连接到来事件
- TcpConnection：TCP 连接封装器。持有 fd、channel、读写缓冲区 buffer；还持有回调函数 closeCallback、messageCallback 用于执行上层 TcpServer 的连接关闭回调和消息处理回调，当连接关闭、可读时触发回调。callback 用于处理 channel 读写事件和关闭事件
- TcpServer：TCP 服务器。持有 acceptor 和管理 tcpConnections；还持有回调函数 connectionCallback、messageCallback 用于执行上层应用的连接建立、断开回调和消息处理回调，当有新连接建立（断开）、收到消息时触发调用。callback 用于处理 acceptor 的新连接事件和 tcpConnection 的断开、消息处理事件
- llhttp：HTTP 解析器（第三方库，Node.js 使用的 HTTP 解析器）。用于解析 HTTP 请求报文。
- HttpResponse：HTTP 响应封装器。用于构造 HTTP 响应报文。
- HttpRequest：HTTP 请求封装器。用于封装 HTTP 请求报文数据，类似一个数据结构体。
- HttpContext：HTTP 解析器。持有 llhttp 解析器实例、HttpRequest 实例，用于解析 HTTP 请求报文，并将解析结果存入 HttpRequest 实例中。
- HttpServer：HTTP 服务器。持有 TcpServer 实例；还持有回调函数 httpCallback 用于执行上层应用的 HTTP 请求处理回调，当收到 HTTP 请求时触发调用。callback（on_message、on_connection） 用于处理 TcpServer 的消息处理事件和连接建立、断开事件

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

## TcpServer 模块 UML 类图

```mermaid

%%{init: {
    "theme": "default",
    "themeVariables": {
        "fontFamily": "Times New Roman",
        "fontSize": "20px"
    }
}}%%

classDiagram
    direction TD

    subgraph Reactor 核心
        class EventLoop {
            -std::unique_ptr<Poller> poller
            +loop()
            +update_channel(Channel* channel)
            +remove_channel(Channel* channel)
        }

        class EPollPoller {
            -int epollfd_
            -const int eventListSize = 16
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
            -std::function readCallback
            -std::function writeCallback
            -std::function closeCallback
            -std::function errorCallback
            
            -handle_read()
            +set_read_callback(std::function cb)
            -handle_write()
            +set_write_callback(std::function cb)
            -handle_close()
            +set_close_callback(std::function cb)
            -handle_error()
            +set_error_callback(std::functioncb)
        }
    end

    subgraph TCP 网络层
        class TcpServer {
            -EventLoop* loop
            -UniquePtr acceptor
            -ConnectionMap connections
            
            -MessageCallback messageCallback
            
            +set_message_callback(MessageCallback cb) // 中间者
        }
        
        class Acceptor {
            -EventLoop* loop
            -int listenFd
            -InetAddress listenAddr
            -std::unique_ptr<Channel> channel
            
            -read_callback() // channel 的回调处理函数
            -handle_connect(int connFd)
            +set_connect_callback(std::function cb)
        }

        class TcpConnection {
            -EventLoop* loop
            -int connectFd
            -unique_ptr channel
            -unique_ptr readBuffer
            -unique_ptr writeBuffer
            
            -MessageCallback messageCallback
            -CloseCallback closeCallback
            
            -read_callback()
            -write_callback()
            -close_callback()
            
            -handle_message()
            -handle_close()
            +set_message_callback(MessageCallback _cb)
            +set_close_callback(CloseCallback _cb)
        }

        class Buffer {
            -std::vector~char~ buffer;
            -size_t readIndex;
            -size_t writeIndex;

            +read_from_buffer() std::string 
            +write_to_buffer(const char* data, size_t len)
            +write_to_buffer(const std::string& str)

            +read_from_fd(int fd, int* savedErrno) ssize_t 
            +write_to_fd(int fd, int* savedErrno) ssize_t
        }
    end

    %% -- 继承关系 --
    
    %% -- 组合/聚合关系 (拥有) --
    EventLoop "1" *-- "1" EPollPoller: owns
    EPollPoller "1" --> "n" Channel: tracks
    
    Acceptor "1" *-- "1" Channel: owns
    TcpConnection "1" *-- "1" Channel: owns
    TcpConnection "1" *-- "1" Buffer: owns
    
    TcpServer "1" *-- "1" Acceptor: owns
    TcpServer "1" *-- "n" TcpConnection: manages

```

## Citation

- https://github.com/chenshuo/muduo
- https://github.com/nodejs/llhttp

## References

- 陈硕. 《Linux 多线程服务器编程：使用 muduo C++ 网络库》. 电子工业出版社, 2013.
- [muduo 源码剖析 - bilibili](https://www.bilibili.com/video/BV1nu411Q7Gq?spm_id_from=333.788.videopod.sections&vd_source=5f255b90a5964db3d7f44633d085b6e4)
