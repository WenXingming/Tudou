/**
 * @file ProtobufChannel.cpp
 * @brief 基于 Protobuf RPC 协议的二进制客户端传输管道实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "ProtobufChannel.h"
#include "tudou/rpc/protocol/RpcCodec.h"
#include "protocol.pb.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace tudou {
namespace rpc {

ProtobufChannel::ProtobufChannel(const std::string& ip, uint16_t port) {
    clientFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd_ < 0) {
        throw std::runtime_error("ProtobufChannel: Failed to create socket");
    }

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    
    if (::inet_pton(AF_INET, ip.c_str(), &servAddr.sin_addr) != 1) {
        ::close(clientFd_);
        throw std::runtime_error("ProtobufChannel: Invalid IP address: " + ip);
    }

    if (::connect(clientFd_, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        ::close(clientFd_);
        throw std::runtime_error("ProtobufChannel: Failed to connect to server " + ip + ":" + std::to_string(port));
    }
}

ProtobufChannel::~ProtobufChannel() {
    if (clientFd_ >= 0) {
        ::close(clientFd_);
    }
}

void ProtobufChannel::CallMethod(const google::protobuf::MethodDescriptor* method,
                                google::protobuf::RpcController* /* controller */,
                                const google::protobuf::Message* request,
                                google::protobuf::Message* response,
                                google::protobuf::Closure* done) {
    uint64_t seq = nextSequenceId_++;

    // 1. 编码 RPC 元数据 (RpcMeta)
    RpcMeta meta;
    meta.set_service_name(method->service()->full_name());
    meta.set_method_name(method->name());
    std::string metaRaw;
    if (!meta.SerializeToString(&metaRaw)) {
        throw std::runtime_error("ProtobufChannel: Failed to serialize RpcMeta");
    }

    // 2. 编码业务数据 (Request Body)
    std::string bodyRaw;
    if (!request->SerializeToString(&bodyRaw)) {
        throw std::runtime_error("ProtobufChannel: Failed to serialize Request Message");
    }

    // 3. 打包成符合 Tudou 二进制协议帧的 Buffer 字节流
    Buffer writeBuf;
    RpcCodec::encode(&writeBuf, RpcMessageType::Request, seq, metaRaw, bodyRaw);
    std::string bytesToSend = writeBuf.read_from_buffer();

    // 4. 同步阻塞发送所有数据
    size_t totalSent = 0;
    while (totalSent < bytesToSend.size()) {
        ssize_t n = ::write(clientFd_, bytesToSend.data() + totalSent, bytesToSend.size() - totalSent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("ProtobufChannel: Failed to write bytes to socket");
        }
        totalSent += n;
    }

    // 5. 循环同步阻塞读取网络字节流直到解码成功
    Buffer readBuf;
    char temp[512];
    RpcHeader respHeader;
    std::string respMetaRaw;
    std::string respBodyRaw;

    while (true) {
        ssize_t nr = ::read(clientFd_, temp, sizeof(temp));
        if (nr <= 0) {
            if (nr < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("ProtobufChannel: Connection closed by remote while waiting for response");
        }

        readBuf.write_to_buffer(temp, nr);

        RpcCodec::DecodeResult result = RpcCodec::decode(&readBuf, respHeader, respMetaRaw, respBodyRaw);
        if (result == RpcCodec::DecodeResult::Success) {
            // 校验会话 Sequence ID 匹配度
            if (respHeader.sequenceId != seq) {
                spdlog::warn("ProtobufChannel: Ignored response sequence id mismatch. Expected={}, Got={}", seq, respHeader.sequenceId);
                continue; // 忽略不匹配的旧包，继续等待当前包
            }

            // 成功捕获回包，执行反序列化还原入出参 response
            if (!response->ParseFromString(respBodyRaw)) {
                throw std::runtime_error("ProtobufChannel: Failed to parse Response Message");
            }

            // 派发 done 回调
            if (done) {
                done->Run();
            }
            break; // 顺利闭环，退出同步等待
        }
        else if (result == RpcCodec::DecodeResult::Error) {
            throw std::runtime_error("ProtobufChannel: Protocol error from server");
        }
        // 如果是 HalfPack 或 Empty，继续循环 read
    }
}

} // namespace rpc
} // namespace tudou
