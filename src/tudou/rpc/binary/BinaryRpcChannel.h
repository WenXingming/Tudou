/**
 * @file BinaryRpcChannel.h
 * @brief 基于二进制 RPC 协议支持单连接多路复用的客户端通道声明
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
#include "tudou/rpc/Coroutine.h"
#include "tudou/tcp/Buffer.h"

class Channel;
class EventLoop;

namespace tudou {
namespace rpc {
namespace binary {

class BinaryRpcChannel : public google::protobuf::RpcChannel {
public:
    /**
     * @brief 构造函数，建立连接并拉起后台接收线程
     */
    BinaryRpcChannel(const std::string& ip, uint16_t port);
    
    /**
     * @brief 构造函数，绑定 EventLoop 并启用非阻塞事件驱动模型（协程版）
     */
    BinaryRpcChannel(EventLoop* loop, const std::string& ip, uint16_t port);
    
    /**
     * @brief 析构函数，优雅释放后台线程并清理挂起请求
     */
    ~BinaryRpcChannel() override;

    // 禁用拷贝构造和赋值
    BinaryRpcChannel(const BinaryRpcChannel&) = delete;
    BinaryRpcChannel& operator=(const BinaryRpcChannel&) = delete;

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
        std::shared_ptr<Coroutine> coroutine; // 关联的协程上下文
        std::exception_ptr exception;         // 缓存的异常指针
    };

    /**
     * @brief 后台接收线程的循环体，专职从 Socket 读取字节并进行 BinaryRpcCodec 拆包分发
     */
    void receive_loop();

    /**
     * @brief 底层 Channel 触发可读事件时的非阻塞回调
     */
    void on_read();

    /**
     * @brief 底层 Channel 触发可写事件时的非阻塞回调
     */
    void on_write();

    void write_request_nonblocking(const std::string& data);

    std::string encode_request(const google::protobuf::MethodDescriptor* method,
                               const google::protobuf::Message* request,
                               uint64_t seq);

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

    // 非阻塞 EventLoop 模式专有变量
    EventLoop* loop_ = nullptr;
    std::unique_ptr<Channel> channel_;
    Buffer readBuf_;
    std::string writeBuffer_;
    bool connected_ = false;
};

} // namespace binary
} // namespace rpc
} // namespace tudou
