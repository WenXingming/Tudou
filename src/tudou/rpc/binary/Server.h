// ============================================================================
// Server 编排 TCP 收发、连接级分帧和 Protobuf 路由。
// 具体字节解析由 Connection 完成，业务调用由 Router 完成。
// ============================================================================

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "tudou/rpc/binary/Connection.h"
#include "tudou/rpc/binary/Request.h"
#include "tudou/rpc/binary/Router.h"
#include "tudou/tcp/TcpServer.h"

namespace tudou {
namespace rpc {
namespace binary {

class Server {
public:
    Server(const std::string& ip, uint16_t port, int numThreads = 0);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void start();
    void stop();

    void register_service(std::shared_ptr<google::protobuf::Service> service);

    uint16_t get_listen_port() const;

private:
    // TcpServer 依次触发连接建立、收到数据和连接关闭。
    void on_connection(const TcpConnectionPtr& conn);
    void on_message(const TcpConnectionPtr& conn);
    void on_close(const TcpConnectionPtr& conn);

    static bool parse_request(const Frame& frame, Request& request);

private:
    TcpServer tcpServer_;
    Router router_;

    std::mutex connectionsMutex_;
    std::unordered_map<TcpConnection*, std::shared_ptr<Connection>> connections_;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
