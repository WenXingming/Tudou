/**
 * @file ProtobufServer.cpp
 * @brief 基于 Protobuf RPC 协议的二进制 TCP 服务端实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "ProtobufServer.h"
#include "tudou/rpc/protocol/RpcCodec.h"
#include "protocol.pb.h"
#include <spdlog/spdlog.h>

namespace tudou {
namespace rpc {

ProtobufServer::ProtobufServer(const std::string& ip, uint16_t port, int numThreads)
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

ProtobufServer::~ProtobufServer() = default;

void ProtobufServer::start() {
    tcpServer_->start();
    spdlog::info("ProtobufServer: Started listening on {}:{}", tcpServer_->get_ip(), tcpServer_->get_port());
}

void ProtobufServer::stop() {
    tcpServer_->stop();
}

uint16_t ProtobufServer::get_listen_port() const {
    return tcpServer_->get_port();
}

void ProtobufServer::register_service(std::shared_ptr<google::protobuf::Service> service) {
    router_.register_service(std::move(service));
}

void ProtobufServer::on_connection(const TcpConnectionPtr& conn) {
    spdlog::info("ProtobufServer: Client connected, fd={}, peer={}", 
                 conn->get_fd(), conn->get_peer_addr().get_ip_port());
}

void ProtobufServer::on_message(const TcpConnectionPtr& conn) {
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
        RpcCodec::DecodeResult result = RpcCodec::decode(&buf, header, metaRaw, bodyRaw);
        
        if (result == RpcCodec::DecodeResult::Success) {
            // 反序列化 RPC 元信息
            RpcMeta meta;
            if (!meta.ParseFromString(metaRaw)) {
                spdlog::error("ProtobufServer: Failed to parse RpcMeta on fd {}. Closing connection...", conn->get_fd());
                hasCorruptFrame = true;
                break;
            }

            uint64_t sequenceId = header.sequenceId;

            // 派发至反射路由器执行具体业务
            try {
                router_.dispatch(meta.service_name(), meta.method_name(), bodyRaw,
                    [this, conn, sequenceId](const std::string& responseRaw) {
                        Buffer responseBuf;
                        RpcCodec::encode(&responseBuf, RpcMessageType::Response, sequenceId, "", responseRaw);
                        conn->send(responseBuf.read_from_buffer());
                    }
                );
            }
            catch (const std::exception& e) {
                spdlog::error("ProtobufServer: Dispatch exception for {}.{}, error={}", 
                              meta.service_name(), meta.method_name(), e.what());
            }
        }
        else if (result == RpcCodec::DecodeResult::HalfPack || result == RpcCodec::DecodeResult::Empty) {
            break;
        }
        else if (result == RpcCodec::DecodeResult::Error) {
            spdlog::error("ProtobufServer: Decode error on fd {}. Closing connection...", conn->get_fd());
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

void ProtobufServer::on_close(const TcpConnectionPtr& conn) {
    spdlog::info("ProtobufServer: Client disconnected, fd={}", conn->get_fd());
    connectionBuffers_.erase(conn.get());
}

} // namespace rpc
} // namespace tudou
