/**
 * @file JsonRpcServer.h
 * @brief 基于 TCP 传输的 JSON-RPC 2.0 服务端声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "tudou/tcp/TcpServer.h"
#include "tudou/rpc/json/JsonRpcRouter.h"

class JsonRpcServer {
public:
    JsonRpcServer(const std::string& ip, uint16_t port, int numThreads = 0);
    JsonRpcServer(const InetAddress& listenAddr, int numThreads = 0);
    ~JsonRpcServer();

    // 禁用拷贝构造和赋值
    JsonRpcServer(const JsonRpcServer&) = delete;
    JsonRpcServer& operator=(const JsonRpcServer&) = delete;

    /**
     * @brief 启动 TCP 服务端端口监听
     */
    void start();

    /**
     * @brief 停止 TCP 服务端监听并退出主循环
     */
    void stop() {
        tcpServer_->stop();
    }

    /**
     * @brief 获取服务端实际绑定的监听端口
     */
    uint16_t get_listen_port() const {
        return tcpServer_->get_port();
    }

    /**
     * @brief 向底层的 RPC 协议服务注册业务方法处理器
     */
    void register_method(const std::string& name, JsonRpcRouter::RpcHandler handler);

private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

    std::unique_ptr<TcpServer> tcpServer_;
    JsonRpcRouter router_;

    // 每个连接的应用层接收缓冲区，用于解决 TCP 粘包和半包
    std::unordered_map<TcpConnection*, std::string> connectionBuffers_;
};
