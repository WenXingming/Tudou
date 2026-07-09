/**
 * @file JsonRpcClient.h
 * @brief 基于 TCP 传输的 JSON-RPC 2.0 C++ 客户端声明
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace tudou {
namespace rpc {

class JsonRpcClient {
public:
    /**
     * @brief 构造函数，建立到 JSON-RPC 服务端的 TCP 连接
     * @param ip 服务端 IP 地址
     * @param port 服务端监听端口
     */
    JsonRpcClient(const std::string& ip, uint16_t port);
    
    /**
     * @brief 析构函数，主动关闭网络套接字
     */
    ~JsonRpcClient();

    // 禁用拷贝构造和赋值
    JsonRpcClient(const JsonRpcClient&) = delete;
    JsonRpcClient& operator=(const JsonRpcClient&) = delete;

    /**
     * @brief 同步发起 JSON-RPC 远程过程调用。
     * @param method 调用方法名称 (例如 "add")
     * @param params 调用入参。可以是 json::array 或者 json::object，若无参则传入 nullptr
     * @return 返回执行成功后的 json::result payload。
     *         若服务端返回 error 错误包，则抛出 std::runtime_error。
     */
    nlohmann::json call(const std::string& method, const nlohmann::json& params = nullptr);

private:
    int clientFd_ = -1;
    uint64_t nextSequenceId_ = 1;
};

} // namespace rpc
} // namespace tudou
