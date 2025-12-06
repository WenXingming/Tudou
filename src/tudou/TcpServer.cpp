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
    acceptor->set_connect_callback(std::bind(&TcpServer::on_connect, this, std::placeholders::_1)); // 或者可以使用 lambda
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
    loop->loop();
}

void TcpServer::send_message(int fd, const std::string& msg) {
    auto findIt = connections.find(fd);
    if (findIt != connections.end()) {
        auto conn = findIt->second;
        conn->send(msg);
    }
    else {
        spdlog::error("TcpServer::send(). connection not found, fd: {}", fd);
    }
}

void TcpServer::on_connect(const int connFd) {
    spdlog::info("New connection created. fd is: {}", connFd);

    auto conn = std::make_shared<TcpConnection>(loop.get(), connFd);
    conn->init_channel(); // 绑定 shared_from_this，设置 tie，防止回调过程中 TcpConnection 对象被析构

    conn->set_message_callback(
        std::bind(&TcpServer::on_message, this, std::placeholders::_1)
    );
    conn->set_close_callback([this](const std::shared_ptr<TcpConnection>& _conn) {
        this->on_close(_conn);
        });
    connections[connFd] = conn;

    // 触发上层回调。上层可以设置连接建立时的逻辑
    handle_connection_callback(connFd);
}

void TcpServer::on_message(const std::shared_ptr<TcpConnection>& conn) {
    // TcpServer 本不处理具体消息逻辑，只做中间者嵌套调用，转发给上层业务逻辑。
    // 但是为了类之间的屏蔽，TcpServer 需要向上提供 fd 和 msg，而不是 TcpConnection 对象本身
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
    assert(this->connectionCallback != nullptr);
    this->connectionCallback(fd);
}

void TcpServer::handle_message_callback(int fd, const std::string& msg) {
    assert(this->messageCallback != nullptr);
    this->messageCallback(fd, msg);
}

void TcpServer::handle_close_callback(int fd) {
    assert(this->closeCallback != nullptr);
    this->closeCallback(fd);
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
