/**
 * @file BinaryRpcServer.cpp
 * @brief 基于二进制 RPC 协议的二进制 TCP 服务端实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "BinaryRpcServer.h"
#include "tudou/rpc/binary/BinaryRpcCodec.h"
#include "binary_rpc.pb.h"
#include <spdlog/spdlog.h>

namespace tudou {
namespace rpc {
namespace binary {

BinaryRpcServer::BinaryRpcServer(const std::string& ip, uint16_t port, int numThreads)
    : tcpServer_(std::make_unique<TcpServer>(ip, port, numThreads > 0 ? numThreads - 1 : 0)) {
    
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

BinaryRpcServer::~BinaryRpcServer() = default;

void BinaryRpcServer::start() {
    tcpServer_->start();
    spdlog::info("BinaryRpcServer: Started listening on {}:{}", tcpServer_->get_ip(), tcpServer_->get_port());
}

void BinaryRpcServer::stop() {
    tcpServer_->stop();
}

uint16_t BinaryRpcServer::get_listen_port() const {
    return tcpServer_->get_port();
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

    // 获取并追加新数据入连接关联缓存
    std::string& cache = connectionBuffers_[conn.get()];
    cache.append(data);

    // 载入临时解包 Buffer
    Buffer buf;
    buf.write_to_buffer(cache.data(), cache.size());

    RpcHeader header;
    std::string metaRaw;
    std::string bodyRaw;
    bool hasCorruptFrame = false;

    while (true) {
        BinaryRpcCodec::DecodeResult result = BinaryRpcCodec::decode(&buf, header, metaRaw, bodyRaw);
        
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
        conn->force_close();
        connectionBuffers_.erase(conn.get());
    } else {
        // 保存未消费的半包流数据
        cache = buf.read_from_buffer();
    }
}

void BinaryRpcServer::on_close(const TcpConnectionPtr& conn) {
    spdlog::info("BinaryRpcServer: Client disconnected, fd={}", conn->get_fd());
    connectionBuffers_.erase(conn.get());
}

} // namespace binary
} // namespace rpc
} // namespace tudou
