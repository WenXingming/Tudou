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

#include "../base/InetAddress.h"
#include "spdlog/spdlog.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"
#include "EventLoopThread.h"
#include "EventLoopThreadPool.h"

TcpServer::TcpServer(std::string ip, uint16_t port, size_t ioLoopNum) :
    loopThreadPool(new EventLoopThreadPool("TcpServerLoopPool", ioLoopNum)),
    ip(std::move(ip)),
    port(port),
    acceptor(nullptr),
    connections(),
    connectionCallback(nullptr),
    messageCallback(nullptr),
    closeCallback(nullptr) {

    EventLoop* mainLoop = loopThreadPool->get_main_loop();
    InetAddress listenAddr(this->ip, this->port);
    assert(mainLoop != nullptr);
    acceptor.reset(new Acceptor(mainLoop, listenAddr));
    acceptor->set_connect_callback(std::bind(&TcpServer::on_connect, this, std::placeholders::_1)); // 或者可以使用 lambda
}

TcpServer::~TcpServer() {
    spdlog::info("TcpServer::~TcpServer() called.");
}

void TcpServer::set_connection_callback(ConnectionCallback cb) {
    this->connectionCallback = cb;
}

void TcpServer::set_message_callback(MessageCallback cb) {
    this->messageCallback = cb;
}

void TcpServer::set_close_callback(CloseCallback cb) {
    this->closeCallback = cb;
}

void TcpServer::start() {
    EventLoop* mainLoop = loopThreadPool->get_main_loop();
    assert(mainLoop != nullptr);
    mainLoop->loop(); // 启动监听事件循环
}

void TcpServer::send_message(int fd, const std::string& msg) {
    std::shared_ptr<TcpConnection> conn;
    {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        auto findIt = connections.find(fd);
        if (findIt != connections.end()) {
            conn = findIt->second; // 拿到一份 shared_ptr 副本，锁外使用
        }
    }

    if (conn) {
        conn->send(msg);
    }
    else {
        spdlog::error("TcpServer::send(). connection not found, fd: {}", fd);
    }
}

void TcpServer::on_connect(const int connFd) {
    spdlog::info("TcpServer::on_connect(), connFd={}", connFd);
    assert(connFd >= 0 && connFd < 1000000); // 简单 sanity check

    EventLoop* mainLoop = loopThreadPool->get_main_loop();
    assert(mainLoop != nullptr);
    mainLoop->assert_in_loop_thread();

    // 选择一个 EventLoop 来管理该连接，轮询选择（通常是 ioLoop，除非只有一个 mainLoop）
    EventLoop* ioLoop = loopThreadPool->get_next_loop();
    assert(ioLoop != nullptr);

    // 等待直到切换到目标线程执行初始化。Fixme: 必须让初始化在目标线程执行，否则会有执行顺序问题，还没初始化好就触发事件回调了
    // 在对应的 IO 线程中执行 TcpConnection 的初始化操作
    ioLoop->run_in_loop([this, connFd, ioLoop]() {
        spdlog::info("TcpServer lambda start, connFd={}", connFd);
        assert(connFd >= 0 && connFd < 1000000); // 简单 sanity check

        auto conn = std::make_shared<TcpConnection>(ioLoop, connFd);
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
        handle_connection_callback(connFd);
        });
}

void TcpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    // TcpServer 本不处理具体消息逻辑，只做中间者嵌套调用，转发给上层业务逻辑。但是为了类之间的屏蔽，设计 TcpServer 向上提供 fd 和 msg，而不是 TcpConnection 对象本身
    int fd = conn->get_fd();
    std::string msg = conn->receive();
    handle_message_callback(fd, msg);
}

void TcpServer::on_close(const std::shared_ptr<TcpConnection>& conn) {
    remove_connection(conn);

    // 触发上层回调。上层可以设置连接关闭时的逻辑
    int fd = conn->get_fd();
    handle_close_callback(fd);
}

void TcpServer::handle_connection_callback(int fd) {
    if (this->connectionCallback == nullptr) {
        spdlog::warn("TcpServer::handle_connection_callback(). connectionCallback is nullptr, fd: {}", fd);
    }
    else {
        this->connectionCallback(fd);
    }
}

void TcpServer::handle_message_callback(int fd, const std::string& msg) {
    assert(this->messageCallback != nullptr);
    this->messageCallback(fd, msg);
}

void TcpServer::handle_close_callback(int fd) {
    if (this->closeCallback == nullptr) {
        spdlog::warn("TcpServer::handle_close_callback(). closeCallback is nullptr, fd: {}", fd);
    }
    else {
        this->closeCallback(fd);
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

// EventLoop* TcpServer::get_loop() {
//     EventLoop* loop;
//     EventLoop* ioLoop = ioLoopThreadPool->get_next_loop();
//     if (ioLoop == nullptr) {
//         loop = mainLoop.get();
//         spdlog::warn("TcpServer::on_connect(). No IO loop available, use main loop instead.");
//     }
//     else {
//         loop = ioLoop;
//     }

//     assert(loop != nullptr);
//     return loop;
// }
