/**
 * @file TcpServer.h
 * @brief TCP 服务器：管理 Acceptor 与 TcpConnection，会话创建、回调接线与连接生命周期管理。
 * @author
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "TcpServer.h"
#include "EventLoop.h"
#include "Acceptor.h"
#include "TcpConnection.h"
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <iostream>
#include <cassert>
#include <functional>
#include "../base/InetAddress.h"
#include "../base/Log.h"

TcpServer::TcpServer(EventLoop* _loop, const InetAddress& _listenAddr, TcpServer::MessageCallback _messageCallback)
    : loop(_loop)
    , acceptor(new Acceptor(_loop, _listenAddr, nullptr)) // 先传入空指针，后面再设置回调函数
    , connections()
    , messageCallback(std::move(_messageCallback)) {

    // 设置 Acceptor 的回调函数。使用 bind 绑定成员函数作为回调
    acceptor->subscribe_on_connect(
        std::bind(&TcpServer::on_connect_callback, this, std::placeholders::_1) // 或者可以使用 lambda
    );
}

TcpServer::~TcpServer() {
    this->connections.clear();
}

void TcpServer::start() {
    // EventLoop 开始循环时，Acceptor 会自动监听
}

void TcpServer::on_connect_callback(int connFd) {
    LOG::LOG_DEBUG("New connection created. fd is: %d", connFd);

    // 初始化 conn。设置业务层回调函数，callback 是由业务传入的，TcpServer 并不实现 callback 只是做中间者
    auto conn = std::make_shared<TcpConnection>(loop, connFd);
    conn->set_message_callback(this->messageCallback);
    conn->set_close_callback(std::bind(&TcpServer::close_callback, this, std::placeholders::_1));

    connections[connFd] = conn;
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

void TcpServer::set_message_callback(MessageCallback cb) {
    this->messageCallback = cb;
}
