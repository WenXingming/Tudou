// ============================================================================
// 基于 TCP 传输的 JSON-RPC 2.0 服务端。
// 绑定底层的 TcpServer，负责按 \n 切包拆包，并调度 JsonRpcRouter 完成方法响应。
// ============================================================================

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "tudou/rpc/JsonRpcRouter.h"
#include "tudou/tcp/TcpServer.h"

class JsonRpcServer {
public:
    JsonRpcServer(const std::string& ip, uint16_t port, int numThreads = 0);
    JsonRpcServer(const InetAddress& listenAddr, int numThreads = 0);
    ~JsonRpcServer();

    JsonRpcServer(const JsonRpcServer&) = delete;
    JsonRpcServer& operator=(const JsonRpcServer&) = delete;

    void start();
    void stop() { tcpServer_.stop(); }

    void register_method(const std::string& name, JsonRpcRouter::RpcHandler handler);

    uint16_t get_listen_port() const { return tcpServer_.get_port(); }

private:
    std::vector<std::string> extract_lines(TcpConnection* connKey, const std::string& data);

    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

private:
    TcpServer tcpServer_;
    JsonRpcRouter router_;

    std::mutex mutex_;
    std::unordered_map<TcpConnection*, std::string> connectionBuffers_;
};
