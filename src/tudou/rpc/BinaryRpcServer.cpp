// ============================================================================
// BinaryRpcServer 负责连接级字节流处理：追加到连接 Buffer、逐帧解码、路由
// 完整请求，并把响应编码后写回。业务方法和 Protobuf 反射由 Router 负责。
// ============================================================================

#include "BinaryRpcServer.h"
#include "tudou/rpc/BinaryRpcCodec.h"
#include "BinaryRpc.pb.h"

#include <utility>

#include <spdlog/spdlog.h>

namespace tudou {
namespace rpc {
namespace binary {

BinaryRpcServer::BinaryRpcServer(const std::string& ip, uint16_t port, int numThreads)
    : tcpServer_(ip, port, numThreads > 0 ? numThreads - 1 : 0),
      router_(),
      connectionMutex_(),
      connectionBuffers_() {
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

BinaryRpcServer::~BinaryRpcServer() = default;

void BinaryRpcServer::start() {
    spdlog::info("BinaryRpcServer: Starting listener on {}:{}", tcpServer_.get_ip(), tcpServer_.get_port());
    tcpServer_.start();
}

void BinaryRpcServer::stop() {
    tcpServer_.stop();
}

uint16_t BinaryRpcServer::get_listen_port() const {
    return tcpServer_.get_port();
}

void BinaryRpcServer::register_service(std::shared_ptr<google::protobuf::Service> service) {
    router_.register_service(std::move(service));
}

void BinaryRpcServer::on_connection(const TcpConnectionPtr& conn) {
    spdlog::info("BinaryRpcServer: Client connected, fd={}, peer={}", 
                 conn->get_fd(), conn->get_peer_addr().get_ip_port());
}

void BinaryRpcServer::on_message(const TcpConnectionPtr& conn) {
    std::string data = conn->receive();
    if (data.empty()) {
        return;
    }

    // 连接表需要跨 IO 线程同步；同一连接的 Buffer 只在所属 IO 线程中解析。
    std::shared_ptr<Buffer> buffer;
    {
        std::lock_guard<std::mutex> lock(connectionMutex_);
        std::shared_ptr<Buffer>& connectionBuffer = connectionBuffers_[conn.get()];
        if (!connectionBuffer) {
            connectionBuffer = std::make_shared<Buffer>();
        }
        buffer = connectionBuffer;
    }
    buffer->write_to_buffer(data);

    RpcHeader header;
    std::string metaRaw;
    std::string bodyRaw;
    bool hasCorruptFrame = false;

    while (true) {
        BinaryRpcCodec::DecodeResult result = BinaryRpcCodec::decode(buffer.get(), header, metaRaw, bodyRaw);
        
        if (result == BinaryRpcCodec::DecodeResult::Success) {
            // 反序列化 RPC 元信息
            RpcMeta meta;
            if (!meta.ParseFromString(metaRaw)) {
                spdlog::error("BinaryRpcServer: Failed to parse RpcMeta on fd {}. Closing connection...", conn->get_fd());
                hasCorruptFrame = true;
                break;
            }

            uint64_t sequenceId = header.sequenceId;

            // 派发至反射路由器执行具体业务
            try {
                router_.dispatch(meta.service_name(), meta.method_name(), bodyRaw,
                    [this, conn, sequenceId](const std::string& responseRaw) {
                        Buffer responseBuf;
                        BinaryRpcCodec::encode(&responseBuf, RpcMessageType::Response, sequenceId, "", responseRaw);
                        conn->send(responseBuf.read_from_buffer());
                    }
                );
            }
            catch (const std::exception& e) {
                spdlog::error("BinaryRpcServer: Dispatch exception for {}.{}, error={}", 
                              meta.service_name(), meta.method_name(), e.what());
            }
        }
        else if (result == BinaryRpcCodec::DecodeResult::HalfPack || result == BinaryRpcCodec::DecodeResult::Empty) {
            break;
        }
        else if (result == BinaryRpcCodec::DecodeResult::Error) {
            spdlog::error("BinaryRpcServer: Decode error on fd {}. Closing connection...", conn->get_fd());
            hasCorruptFrame = true;
            break;
        }
    }

    if (hasCorruptFrame) {
        {
            std::lock_guard<std::mutex> lock(connectionMutex_);
            connectionBuffers_.erase(conn.get());
        }
        conn->force_close();
    }
}

void BinaryRpcServer::on_close(const TcpConnectionPtr& conn) {
    spdlog::info("BinaryRpcServer: Client disconnected, fd={}", conn->get_fd());
    std::lock_guard<std::mutex> lock(connectionMutex_);
    connectionBuffers_.erase(conn.get());
}

} // namespace binary
} // namespace rpc
} // namespace tudou
