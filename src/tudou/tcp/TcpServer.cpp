/**
 * @file TcpServer.cpp
 * @brief TCP 服务器：管理 Acceptor 与 TcpConnection
 * @author WenXingming
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "TcpServer.h"

#include <cassert>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "base/InetAddress.h"
#include "spdlog/spdlog.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"
#include "EventLoopThread.h"
#include "EventLoopThreadPool.h"

TcpServer::TcpServer(std::string _ip, uint16_t _port, size_t _ioLoopNum) :
    loopThreadPool(new EventLoopThreadPool("TcpServerLoopPool", _ioLoopNum)),
    ip(std::move(_ip)),
    port(_port),
    acceptor(nullptr),
    connections(),
    connectionsMutex(),
    connectionCallback(nullptr),
    messageCallback(nullptr),
    closeCallback(nullptr) {
    
    EventLoop* mainLoop = loopThreadPool->get_main_loop();
    InetAddress listenAddr(this->ip, this->port);
    if(mainLoop == nullptr) {
        spdlog::critical("TcpServer::TcpServer(). mainLoop is nullptr.");
        assert(false);
    }
    acceptor.reset(new Acceptor(mainLoop, listenAddr));
    acceptor->set_connect_callback(std::bind(&TcpServer::on_connect, this, std::placeholders::_1)); // 传递 Acceptor 引用

    spdlog::debug("TcpServer::TcpServer() called, ip: {}, port: {}, ioLoopNum: {}", ip, port, _ioLoopNum);
}

TcpServer::~TcpServer() {
    spdlog::debug("TcpServer::~TcpServer() called.");
}

void TcpServer::set_connection_callback(ConnectionCallback cb) {
    this->connectionCallback = std::move(cb);
}

void TcpServer::set_message_callback(MessageCallback cb) {
    this->messageCallback = std::move(cb);
}

void TcpServer::set_close_callback(CloseCallback cb) {
    this->closeCallback = std::move(cb);
}

void TcpServer::start() {
    EventLoop* mainLoop = loopThreadPool->get_main_loop();
    if(mainLoop == nullptr) {
        spdlog::critical("TcpServer::start(). mainLoop is nullptr.");
        assert(false);
    }
    mainLoop->loop(); // 启动监听事件循环，开启服务器
}

void TcpServer::on_connect(Acceptor& acceptor) {
    // 创建连接时确保在 mainLoop 线程调用 on_connect
    assert_in_main_loop_thread();
    
    // 通过 Acceptor 接口获取新连接信息
    int connFd = acceptor.get_accepted_fd();
    const InetAddress& peerAddr = acceptor.get_accepted_peer_addr();

    spdlog::info("TcpServer: New connection from {} on fd {}", peerAddr.get_ip_port(), connFd);

    // 选择一个 EventLoop 来管理该连接，轮询选择（通常是 ioLoop，除非只有一个 mainLoop）
    EventLoop* ioLoop = select_loop();

    // 切换到目标线程执行初始化。Fixme: 必须让初始化在目标线程执行，否则可能会有执行顺序问题，还没初始化好就触发事件回调了（此时可能 callback 还未设置）
    // 在对应的 IO 线程中执行 TcpConnection 的初始化操作
    ioLoop->run_in_loop([this, connFd, ioLoop, peerAddr]() {
        // 获取本地地址信息
        sockaddr_in localSockAddr;
        socklen_t addrLen = sizeof(localSockAddr);
        if (::getsockname(connFd, (sockaddr*)&localSockAddr, &addrLen) < 0) {
            spdlog::error("TcpServer::on_connect(). getsockname error, errno: {}", errno);
        }
        InetAddress localAddr(localSockAddr);
        
        auto conn = std::make_shared<TcpConnection>(ioLoop, connFd, localAddr, peerAddr);
        conn->set_message_callback(
            std::bind(&TcpServer::on_message, this, std::placeholders::_1)
        );
        conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& _conn) {
            this->on_close(_conn);
            });
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connections[connFd] = conn;
        }
        conn->connection_establish(); // 绑定 shared_from_this，设置 tie，防止回调过程中 TcpConnection 对象被析构

        // 触发上层回调。上层可以设置连接建立时的逻辑
        handle_connection_callback(conn);
        });
}

void TcpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    // TcpServer 转发消息给上层业务逻辑，直接传递 conn 对象
    // 业务层通过 conn->receive() 主动获取数据
    handle_message_callback(conn);
}

void TcpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    remove_connection(conn);

    // 触发上层回调。上层可以设置连接关闭时的逻辑
    handle_close_callback(conn);
}

void TcpServer::handle_connection_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->connectionCallback == nullptr) {
        spdlog::warn("TcpServer::handle_connection_callback(). connectionCallback is nullptr, fd: {}", conn->get_fd());
    }
    else {
        this->connectionCallback(conn);
    }
}

void TcpServer::handle_message_callback(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->messageCallback != nullptr);
    this->messageCallback(conn);
}

void TcpServer::handle_close_callback(const std::shared_ptr<TcpConnection>& conn) {
    if (this->closeCallback == nullptr) {
        spdlog::warn("TcpServer::handle_close_callback(). closeCallback is nullptr, fd: {}", conn->get_fd());
    }
    else {
        this->closeCallback(conn);
    }
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    // 在相应的 IO 线程中执行删除操作
    EventLoop* loop = conn->get_loop();
    if (loop->is_in_loop_thread()) {
        // 如果已经在对应的线程中，直接删除
        int fd = conn->get_fd();
        bool erased = false;
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            auto findIt = connections.find(fd);
            if (findIt != connections.end()) {
                connections.erase(findIt);
                erased = true;
            }
        }

        if (!erased) {
            spdlog::error("TcpServer::remove_connection(). connection not found, fd: {}", fd);
        }
    }
    else {
        // 切换到对应的线程中删除
        loop->run_in_loop([this, conn]() {
            int fd = conn->get_fd();
            bool erased = false;
            {
                std::lock_guard<std::mutex> lock(connectionsMutex);
                auto findIt = connections.find(fd);
                if (findIt != connections.end()) {
                    connections.erase(findIt);
                    erased = true;
                }
            }

            if (!erased) {
                spdlog::error("TcpServer::remove_connection(). connection not found, fd: {}", fd);
            }
            });
    }
}

void TcpServer::assert_in_main_loop_thread() const {
    // 创建连接时确保在 mainLoop 线程调用 on_connect
    EventLoop* mainLoop = loopThreadPool->get_main_loop();
    if (mainLoop == nullptr) { // 不太可能发生，只是防御性编程。为了提高效率可以注释掉
        spdlog::critical("TcpServer::on_connect(). mainLoop is nullptr.");
        assert(false);
    }
    mainLoop->assert_in_loop_thread();
}

EventLoop* TcpServer::select_loop() const {
    EventLoop* ioLoop = loopThreadPool->get_next_loop();
    if (ioLoop == nullptr) {
        spdlog::critical("TcpServer::on_connect(). ioLoop is nullptr.");
        assert(false);
    }
    return ioLoop;
}
