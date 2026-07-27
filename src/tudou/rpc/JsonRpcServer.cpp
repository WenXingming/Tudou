// ============================================================================
// 基于 TCP 传输的 JSON-RPC 2.0 服务端实现。
// ============================================================================

#include "JsonRpcServer.h"

#include <utility>

#include <spdlog/spdlog.h>

JsonRpcServer::JsonRpcServer(const std::string& ip, uint16_t port, int numThreads)
    : tcpServer_(ip, port, numThreads > 0 ? numThreads - 1 : 0) {

    tcpServer_.set_connection_callback([this](const TcpConnectionPtr& conn) {
        on_connection(conn);
        });

    tcpServer_.set_message_callback([this](const TcpConnectionPtr& conn) {
        on_message(conn);
        });

    tcpServer_.set_close_callback([this](const TcpConnectionPtr& conn) {
        on_close(conn);
        });
}

JsonRpcServer::JsonRpcServer(const InetAddress& listenAddr, int numThreads)
    : JsonRpcServer(listenAddr.get_ip(), listenAddr.get_port(), numThreads) {
}

JsonRpcServer::~JsonRpcServer() = default;

void JsonRpcServer::start() {
    tcpServer_.start();
    spdlog::info("JsonRpcServer: Started listening on {}:{}", tcpServer_.get_ip(), tcpServer_.get_port());
}

void JsonRpcServer::register_method(const std::string& name, JsonRpcRouter::RpcHandler handler) {
    router_.register_method(name, std::move(handler));
}

std::vector<std::string> JsonRpcServer::extract_lines(TcpConnection* connKey, const std::string& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string& connBuf = connectionBuffers_[connKey];
    connBuf.append(data);

    std::vector<std::string> lines;
    while (true) {
        size_t pos = connBuf.find('\n');
        if (pos == std::string::npos) {
            break;
        }

        std::string requestStr = connBuf.substr(0, pos);
        connBuf.erase(0, pos + 1);

        if (!requestStr.empty()) {
            lines.push_back(std::move(requestStr));
        }
    }
    return lines;
}

void JsonRpcServer::on_connection(const TcpConnectionPtr& conn) {
    spdlog::info("JsonRpcServer: Client connected, fd={}, peer={}", conn->get_fd(), conn->get_peer_addr().get_ip_port());
}

void JsonRpcServer::on_message(const TcpConnectionPtr& conn) {
    std::string data = conn->receive();
    if (data.empty()) {
        return;
    }

    // 面对的客户端不一定都是我们的  JsonRpcClient，第三方 Python/Go 脚本、Node.js 客户端或支持 Pipeline（流水线并发发送） 的客户端
    std::vector<std::string> requests = extract_lines(conn.get(), data);
    for (const auto& requestStr : requests) {
        // router_.dispatch 接收 requestStr （原始 JSON 文本）。解析放在了 router 内部，返回的 responseStr 也是 JSON 文本。
        std::string responseStr = router_.dispatch(requestStr);
        if (!responseStr.empty()) {
            conn->send(responseStr + "\n");
        }
    }
}

void JsonRpcServer::on_close(const TcpConnectionPtr& conn) {
    spdlog::info("JsonRpcServer: Client disconnected, fd={}", conn->get_fd());
    std::lock_guard<std::mutex> lock(mutex_);
    connectionBuffers_.erase(conn.get());
}
