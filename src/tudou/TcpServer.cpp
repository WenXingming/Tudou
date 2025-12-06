/**
 * @file TcpServer.h
 * @brief TCP 服务器：管理 Acceptor 与 TcpConnection，会话创建、回调接线与连接生命周期管理。
 * @author
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "TcpServer.h"

#include <cassert>
#include <functional>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "spdlog/spdlog.h"
#include "../base/InetAddress.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"

TcpServer::TcpServer(std::string ip, uint16_t port)
    : ip(std::move(ip))
    , port(port) {

    this->loop.reset(new EventLoop());

    acceptor.reset(new Acceptor(this->loop.get(), InetAddress(this->ip, this->port)));
    acceptor->set_connect_callback(std::bind(&TcpServer::connect_callback, this, std::placeholders::_1)); // 或者可以使用 lambda
}

TcpServer::~TcpServer() {
    this->connections.clear();
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
    this->loop->loop();
}

void TcpServer::connect_callback(const int connFd) {
    spdlog::debug("New connection created. fd is: {}", connFd);

    // 初始化 conn。设置业务层回调函数，callback 是由业务传入的，TcpServer 并不实现 callback 只是做中间者
    auto conn = std::make_shared<TcpConnection>(loop.get(), connFd);
    conn->set_message_callback(std::bind(&TcpServer::message_callback, this, std::placeholders::_1));
    conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& c) {
        this->close_callback(c);
        });
    connections[connFd] = conn;

    // 触发上层回调。上层可以设置连接建立时的逻辑
    this->handle_connection(conn);
}

void TcpServer::message_callback(const std::shared_ptr<TcpConnection>& conn) {
    // TcpServer 不处理具体消息逻辑，只做中间者嵌套调用，转发给上层业务逻辑
    this->handle_message(conn);
}

void TcpServer::close_callback(const std::shared_ptr<TcpConnection>& conn) {
    this->remove_connection(conn);

    // 触发上层回调。上层可以设置连接关闭时的逻辑
    this->handle_close(conn);
}

void TcpServer::handle_connection(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->connectionCallback != nullptr);
    this->connectionCallback(conn);
}

void TcpServer::handle_message(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->messageCallback != nullptr);
    this->messageCallback(conn);
}

void TcpServer::handle_close(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->closeCallback != nullptr);
    this->closeCallback(conn);
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    auto findIt = connections.find(fd);
    if (findIt != connections.end()) {
        // removeConnection = findIt->second; // 暂存，避免悬空指针
        connections.erase(findIt);
    }
    else {
        spdlog::error("TcpServer::remove_connection(). connection not found, fd: {}", fd);
    }
}
