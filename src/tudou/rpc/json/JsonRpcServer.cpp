/**
 * @file JsonRpcServer.cpp
 * @brief 基于 TCP 传输的 JSON-RPC 2.0 服务端实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "JsonRpcServer.h"
#include <spdlog/spdlog.h>

JsonRpcServer::JsonRpcServer(const std::string& ip, uint16_t port, int numThreads)
    : tcpServer_(new TcpServer(ip, port, numThreads > 0 ? numThreads - 1 : 0)) {

    tcpServer_->set_connection_callback([this](const TcpConnectionPtr& conn) {
        on_connection(conn);
    });

    tcpServer_->set_message_callback([this](const TcpConnectionPtr& conn) {
        on_message(conn);
    });

    tcpServer_->set_close_callback([this](const TcpConnectionPtr& conn) {
        on_close(conn);
    });
}

JsonRpcServer::JsonRpcServer(const InetAddress& listenAddr, int numThreads)
    : JsonRpcServer(listenAddr.get_ip(), listenAddr.get_port(), numThreads) {}

JsonRpcServer::~JsonRpcServer() = default;

void JsonRpcServer::start() {
    tcpServer_->start();
    spdlog::info("JsonRpcServer: Started listening on {}:{}", tcpServer_->get_ip(), tcpServer_->get_port());
}

void JsonRpcServer::register_method(const std::string& name, JsonRpcRouter::RpcHandler handler) {
    router_.register_method(name, std::move(handler));
}

void JsonRpcServer::on_connection(const TcpConnectionPtr& conn) {
    spdlog::info("JsonRpcServer: Client connected, fd={}, peer={}", conn->get_fd(), conn->get_peer_addr().get_ip_port());
}

void JsonRpcServer::on_message(const TcpConnectionPtr& conn) {
    std::string data = conn->receive();
    if (data.empty()) {
        return;
    }

    // 获取该连接对应的应用层缓冲区，并将新数据追加在尾部
    std::string& connBuf = connectionBuffers_[conn.get()];
    connBuf.append(data);

    while (true) {
        // 查找换行符 \n 作为分包边界
        size_t pos = connBuf.find('\n');
        if (pos == std::string::npos) {
            // 没有换行符，说明是半包数据，跳出循环等待下一次可读事件接收后续数据
            break;
        }

        // 提取出一行完整的请求文本
        std::string requestStr = connBuf.substr(0, pos);
        
        // 擦除缓冲区中已被解析处理的数据和换行符本身
        connBuf.erase(0, pos + 1);

        // 过滤空行请求
        if (requestStr.empty()) {
            continue;
        }

        // 调度核心业务处理
        std::string responseStr = router_.dispatch(requestStr);

        // 如果不是 Notification，将响应追加换行符发回对端
        if (!responseStr.empty()) {
            conn->send(responseStr + "\n");
        }
    }
}

void JsonRpcServer::on_close(const TcpConnectionPtr& conn) {
    spdlog::info("JsonRpcServer: Client disconnected, fd={}", conn->get_fd());
    // 清理该连接对应的应用层缓冲区，防止内存泄露
    connectionBuffers_.erase(conn.get());
}
