/**
 * @file UnifiedRpcServer.h
 * @brief 统一的双轨 RPC 服务端声明，同时支持二进制 RPC 与 JSON-RPC
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <memory>
#include <string>
#include <thread>
#include <google/protobuf/service.h>

class JsonRpcServer;

namespace tudou {
namespace rpc {

// 前置声明
namespace binary {
class BinaryRpcServer;
}

class UnifiedRpcServer {
public:
    /**
     * @brief 构造函数，初始化双轨服务端
     * @param ip 监听的 IP 地址
     * @param binaryPort 二进制 RPC 监听端口
     * @param jsonPort JSON-RPC 监听端口
     * @param numThreads I/O 线程池总线程数（由两个服务平分）
     */
    UnifiedRpcServer(const std::string& ip, uint16_t binaryPort, uint16_t jsonPort, int numThreads = 0);
    
    /**
     * @brief 析构函数，优雅释放并停止服务
     */
    ~UnifiedRpcServer();

    // 禁用拷贝构造和赋值
    UnifiedRpcServer(const UnifiedRpcServer&) = delete;
    UnifiedRpcServer& operator=(const UnifiedRpcServer&) = delete;

    /**
     * @brief 注册一个具体的业务 Service 实例。
     *        框架会同时将其挂载至二进制 RPC 路由与动态 JSON-RPC 反射路由上。
     * @param service 继承自 google::protobuf::Service 的具体服务指针
     */
    void register_service(std::shared_ptr<google::protobuf::Service> service);

    /**
     * @brief 启动双轨服务端。
     *        此调用会阻塞当前线程（因为接管了主 Reactor 的 EventLoop）。
     */
    void start();

    /**
     * @brief 停止双轨服务端，关闭底层连接并回收后台线程
     */
    void stop();

    /**
     * @brief 获取绑定的二进制 RPC 服务端口
     */
    uint16_t get_binary_port() const;

    /**
     * @brief 获取绑定的 JSON-RPC 服务端口
     */
    uint16_t get_json_port() const;

private:
    std::unique_ptr<binary::BinaryRpcServer> binaryServer_;
    std::unique_ptr<JsonRpcServer> jsonServer_;
    
    std::thread binaryThread_; // 专职跑二进制服务端 Reactor 的后台线程
    std::string ip_;
    uint16_t binaryPort_;
    uint16_t jsonPort_;
};

} // namespace rpc
} // namespace tudou
