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

#include "../base/InetAddress.h"
#include "../base/Log.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include "TcpConnection.h"

TcpServer::TcpServer(EventLoop* _loop, const InetAddress& _listenAddr)
    : loop(_loop)
    , connections() {

    this->acceptor.reset(new Acceptor(this->loop, _listenAddr));
    acceptor->set_connect_callback(std::bind(&TcpServer::connect_callback, this, std::placeholders::_1)); // 或者可以使用 lambda
}

TcpServer::~TcpServer() {
    this->connections.clear();
}

void TcpServer::connect_callback(int connFd) {
    // LOG::LOG_DEBUG("New connection created. fd is: %d", connFd);

    // 初始化 conn。设置业务层回调函数，callback 是由业务传入的，TcpServer 并不实现 callback 只是做中间者
    auto conn = std::make_shared<TcpConnection>(loop, connFd);
    conn->set_message_callback(std::bind(&TcpServer::message_callback, this, std::placeholders::_1));
    conn->set_close_callback(std::bind(&TcpServer::close_callback, this, std::placeholders::_1));

    connections[connFd] = conn;

    // 触发上层回调
    assert(connectionCallback != nullptr);
    connectionCallback(conn);
}

void TcpServer::message_callback(const std::shared_ptr<TcpConnection>& conn) {
    assert(this->messageCallback != nullptr);
    this->messageCallback(conn);
}

void TcpServer::close_callback(const std::shared_ptr<TcpConnection>& conn) {
    this->remove_connection(conn);
}

void TcpServer::remove_connection(const std::shared_ptr<TcpConnection>& conn) {
    int fd = conn->get_fd();
    auto findIt = connections.find(fd);
    if (findIt != connections.end()) {
        connections.erase(findIt);
    }
    else {
        LOG::LOG_ERROR("TcpServer::remove_connection(). connection not found, fd: %d", fd);
    }
}

void TcpServer::set_connection_callback(ConnectionCallback cb) {
    this->connectionCallback = cb;
}

void TcpServer::set_message_callback(MessageCallback cb) {
    this->messageCallback = cb;
}
