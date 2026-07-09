/**
 * @file ProtobufChannel.h
 * @brief 基于 Protobuf RPC 协议支持单连接多路复用的客户端通道声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <google/protobuf/service.h>
#include <string>
#include <mutex>
#include <unordered_map>
#include <future>
#include <thread>
#include <atomic>
#include <memory>

namespace tudou {
namespace rpc {

class ProtobufChannel : public google::protobuf::RpcChannel {
public:
    /**
     * @brief 构造函数，建立连接并拉起后台接收线程
     */
    ProtobufChannel(const std::string& ip, uint16_t port);
    
    /**
     * @brief 析构函数，优雅释放后台线程并清理挂起请求
     */
    ~ProtobufChannel() override;

    // 禁用拷贝构造和赋值
    ProtobufChannel(const ProtobufChannel&) = delete;
    ProtobufChannel& operator=(const ProtobufChannel&) = delete;

    /**
     * @brief 客户端 Stub 调用的核心纯虚函数覆写。
     *        支持多线程并发调用。内部通过唯一 sequence ID 隔离并利用 promise/future 同步挂起唤醒。
     */
    void CallMethod(const google::protobuf::MethodDescriptor* method,
                    google::protobuf::RpcController* controller,
                    const google::protobuf::Message* request,
                    google::protobuf::Message* response,
                    google::protobuf::Closure* done) override;

private:
    // 单次请求的响应上下文结构
    struct ResponseContext {
        google::protobuf::Message* response;
        std::promise<void> promise;
    };

    /**
     * @brief 后台接收线程的循环体，专职从 Socket 读取字节并进行 RpcCodec 拆包分发
     */
    void receive_loop();

    /**
     * @brief 网络中断或析构时触发，将异常传入所有仍在挂起等待的连接，防止线程永久卡死
     */
    void cleanup_pending_requests(const std::string& reason);

private:
    int clientFd_ = -1;
    std::atomic<uint64_t> nextSequenceId_{1};

    std::atomic<bool> running_{true};
    std::thread receiverThread_;

    std::mutex sendMutex_; // 保护并发 Socket 写入
    std::mutex mapMutex_;  // 保护全局 pending 映射表访问

    // 缓存挂起的请求会话: sequenceId -> ResponseContext
    std::unordered_map<uint64_t, std::shared_ptr<ResponseContext>> pendingRequests_;
};

} // namespace rpc
} // namespace tudou
