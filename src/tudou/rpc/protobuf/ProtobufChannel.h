/**
 * @file ProtobufChannel.h
 * @brief 基于 Protobuf RPC 协议的二进制客户端传输管道声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <google/protobuf/service.h>
#include <string>

namespace tudou {
namespace rpc {

class ProtobufChannel : public google::protobuf::RpcChannel {
public:
    /**
     * @brief 构造函数，建立到 Protobuf RPC 服务端的 TCP 连接
     * @param ip 服务端 IP 地址
     * @param port 服务端监听端口
     */
    ProtobufChannel(const std::string& ip, uint16_t port);
    
    /**
     * @brief 析构函数，回收网络套接字
     */
    ~ProtobufChannel() override;

    // 禁用拷贝构造和赋值
    ProtobufChannel(const ProtobufChannel&) = delete;
    ProtobufChannel& operator=(const ProtobufChannel&) = delete;

    /**
     * @brief 客户端 Stub 调用的核心纯虚函数覆写。
     *        负责编码请求流、发起网络传输、同步阻塞接收二进制回包、反序列化解析并交付数据。
     */
    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

private:
    int clientFd_ = -1;
    uint64_t nextSequenceId_ = 1;
};

} // namespace rpc
} // namespace tudou
