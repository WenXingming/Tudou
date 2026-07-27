// ============================================================================
// BinaryRpcServer 在 TcpServer 之上处理 Tudou 二进制 RPC 帧。
// 每条 TCP 连接各自保留一个 Buffer，以支持半包、粘包和连续请求；完整请求
// 交给 BinaryRpcRouter 分发，响应再编码后写回原连接。
// ============================================================================

#pragma once

#include "tudou/tcp/Buffer.h"
#include "tudou/tcp/TcpServer.h"
#include "tudou/rpc/BinaryRpcRouter.h"
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace tudou {
namespace rpc {
namespace binary {

class BinaryRpcServer {
public:
    BinaryRpcServer(const std::string& ip, uint16_t port, int numThreads = 0);
    ~BinaryRpcServer();

    // 禁用拷贝构造和赋值
    BinaryRpcServer(const BinaryRpcServer&) = delete;
    BinaryRpcServer& operator=(const BinaryRpcServer&) = delete;

    // 生命周期。
    void start();
    void stop();

    // 注册 Protobuf 业务服务。
    void register_service(std::shared_ptr<google::protobuf::Service> service);

    // 查询监听端口。
    uint16_t get_listen_port() const;

private:
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

private:
    TcpServer tcpServer_;
    BinaryRpcRouter router_;

    // 表本身被多个 IO 线程访问；每个 Buffer 在取出后只由所属连接的 IO 线程访问。
    std::mutex connectionMutex_;
    std::unordered_map<TcpConnection*, std::shared_ptr<Buffer>> connectionBuffers_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
