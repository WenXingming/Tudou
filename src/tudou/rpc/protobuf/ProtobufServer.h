/**
 * @file ProtobufServer.h
 * @brief 基于 Protobuf RPC 协议的二进制 TCP 服务端声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include "tudou/tcp/TcpServer.h"
#include "tudou/rpc/protobuf/ProtobufRouter.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace tudou {
namespace rpc {

class ProtobufServer {
public:
    ProtobufServer(const std::string& ip, uint16_t port, int numThreads = 0);
    ~ProtobufServer();

    // 禁用拷贝构造和赋值
    ProtobufServer(const ProtobufServer&) = delete;
    ProtobufServer& operator=(const ProtobufServer&) = delete;

    /**
     * @brief 启动 TCP 服务端，阻塞或在多线程 Reactor 中监听端口
     */
    void start();

    /**
     * @brief 停止 TCP 服务端监听
     */
    void stop();

    /**
     * @brief 获取服务端绑定的真实监听端口
     */
    uint16_t get_listen_port() const;

    /**
     * @brief 注册业务 Service
     */
    void register_service(std::shared_ptr<google::protobuf::Service> service);

private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

private:
    std::unique_ptr<TcpServer> tcpServer_;
    ProtobufRouter router_;

    // 半包缓冲区，key 为连接裸指针，用以处理 TCP 粘包/半包
    std::unordered_map<TcpConnection*, std::string> connectionBuffers_;
};

} // namespace rpc
} // namespace tudou
