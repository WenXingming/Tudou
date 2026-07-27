// ============================================================================
// 基于 TCP 阻塞传输的 JSON-RPC 2.0 客户端。
// 负责建立套接字连接、打包 JSON-RPC 请求并同步接收/校验响应。
// ============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>

#include "base/ScopedFd.h"

namespace tudou {
namespace rpc {

class JsonRpcClient {
public:
    JsonRpcClient(const std::string& ip, uint16_t port);
    ~JsonRpcClient();

    JsonRpcClient(const JsonRpcClient&) = delete;
    JsonRpcClient& operator=(const JsonRpcClient&) = delete;

    // 发起 JSON-RPC 远程调用（同步阻塞），成功返回 result payload，失败抛出 std::runtime_error。
    nlohmann::json call(const std::string& method, const nlohmann::json& params = nullptr);

private:
    static std::string encode_request(const std::string& method, const nlohmann::json& params, uint64_t seq);
    void send_all(const std::string& data);
    std::string read_line();
    static nlohmann::json decode_response(const std::string& line, uint64_t seq);

private:
    ScopedFd clientFd_;
    uint64_t nextSequenceId_ = 1;
    std::string recvBuf_;
};

} // namespace rpc
} // namespace tudou
