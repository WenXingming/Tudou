// ============================================================================
// Server 的消息主流程平铺为：取连接状态、分帧、解析调用、路由、回包。
// ============================================================================

#include "tudou/rpc/binary/Server.h"

#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "binary_rpc.pb.h"
#include "tudou/rpc/binary/FrameCodec.h"

namespace tudou {
namespace rpc {
namespace binary {

// numThreads 包含 TcpServer 自带的 main loop，因此这里只传额外 IO loop 的数量。
Server::Server(const std::string& ip, uint16_t port, int numThreads)
    : tcpServer_(ip, port, numThreads > 0 ? numThreads - 1 : 0),
      router_(),
      connectionsMutex_(),
      connections_() {
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

Server::~Server() = default;

void Server::start() {
    spdlog::info("Server: Starting listener on {}:{}", tcpServer_.get_ip(), tcpServer_.get_port());
    tcpServer_.start();
}

void Server::stop() {
    tcpServer_.stop();
}

void Server::register_service(std::shared_ptr<google::protobuf::Service> service) {
    router_.register_service(std::move(service));
}

uint16_t Server::get_listen_port() const {
    return tcpServer_.get_port();
}

void Server::on_connection(const TcpConnectionPtr& conn) {
    // 每条 TCP 连接独占一个解帧 Buffer，半包不能与其他连接共享。
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_[conn.get()] = std::make_shared<Connection>();
}

void Server::on_message(const TcpConnectionPtr& conn) {
    const std::string data = conn->receive();
    if (data.empty()) {
        return;
    }

    std::shared_ptr<Connection> rpcConnection;
    {
        // 回调可能来自不同 IO 线程。锁只保护查表，解帧和业务调用在锁外执行。
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        const auto it = connections_.find(conn.get());
        if (it == connections_.end()) {
            return;
        }
        rpcConnection = it->second;
    }

    std::vector<Frame> frames;
    if (!rpcConnection->decode(data, frames)) {
        spdlog::error("Server: Invalid frame, fd={}", conn->get_fd());
        conn->force_close();
        return;
    }

    for (const auto& frame : frames) {
        Request request;
        if (!parse_request(frame, request)) {
            spdlog::error("Server: Invalid request, fd={}", conn->get_fd());
            conn->force_close();
            return;
        }

        try {
            const std::string responseBody = router_.dispatch(request);
            // 响应原样携带请求 sequenceId，客户端才能在单连接上匹配等待者。
            const Frame response(FrameType::Response, frame.header.sequenceId, "", responseBody);
            conn->send(FrameCodec::encode(response));
        }
        catch (const std::exception& error) {
            spdlog::error("Server: {}.{}, fd={}, error={}", request.serviceName, request.methodName, conn->get_fd(), error.what());
            conn->force_close();
            return;
        }
    }
}

void Server::on_close(const TcpConnectionPtr& conn) {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.erase(conn.get());
}

bool Server::parse_request(const Frame& frame, Request& request) {
    if (frame.header.type != FrameType::Request) {
        return false;
    }

    CallHead head;
    if (!head.ParseFromString(frame.head)) {
        return false;
    }

    // Frame 只负责传输边界；这里把调用头和 body 提升为 Router 使用的逻辑请求。
    request = Request(head.service_name(), head.method_name(), frame.body);
    return true;
}

} // namespace binary
} // namespace rpc
} // namespace tudou
