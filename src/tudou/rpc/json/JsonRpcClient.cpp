/**
 * @file JsonRpcClient.cpp
 * @brief 基于 TCP 传输的 JSON-RPC 2.0 C++ 客户端实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "JsonRpcClient.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>
#include <sstream>

namespace tudou {
namespace rpc {

JsonRpcClient::JsonRpcClient(const std::string& ip, uint16_t port) {
    clientFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (clientFd_ < 0) {
        throw std::runtime_error("JsonRpcClient: Failed to create socket");
    }

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);
    
    if (::inet_pton(AF_INET, ip.c_str(), &servAddr.sin_addr) != 1) {
        ::close(clientFd_);
        throw std::runtime_error("JsonRpcClient: Invalid IP address: " + ip);
    }

    if (::connect(clientFd_, (struct sockaddr*)&servAddr, sizeof(servAddr)) < 0) {
        ::close(clientFd_);
        throw std::runtime_error("JsonRpcClient: Failed to connect to server " + ip + ":" + std::to_string(port));
    }
}

JsonRpcClient::~JsonRpcClient() {
    if (clientFd_ >= 0) {
        ::close(clientFd_);
    }
}

nlohmann::json JsonRpcClient::call(const std::string& method, const nlohmann::json& params) {
    uint64_t seq = nextSequenceId_++;
    
    // 1. 组装请求对象
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    if (!params.is_null()) {
        request["params"] = params;
    }
    request["id"] = seq;

    std::string requestStr = request.dump() + "\n";

    // 2. 发送请求字节流到网络 Socket
    size_t totalSent = 0;
    while (totalSent < requestStr.size()) {
        ssize_t n = ::write(clientFd_, requestStr.data() + totalSent, requestStr.size() - totalSent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("JsonRpcClient: Failed to send request bytes");
        }
        totalSent += n;
    }

    // 3. 阻塞读取接收流直到遇见行尾分隔符 \n
    std::string responseBuf;
    char temp[512];
    size_t delimiterPos = std::string::npos;

    while (delimiterPos == std::string::npos) {
        ssize_t nr = ::read(clientFd_, temp, sizeof(temp));
        if (nr <= 0) {
            if (nr < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("JsonRpcClient: Connection closed by remote server while waiting for response");
        }
        responseBuf.append(temp, nr);
        delimiterPos = responseBuf.find('\n');
    }

    // 提取完整的一行数据
    std::string responseLine = responseBuf.substr(0, delimiterPos);

    // 4. 解析 JSON 并进行契约正确性断言
    nlohmann::json response;
    try {
        response = nlohmann::json::parse(responseLine);
    }
    catch (const std::exception& e) {
        throw std::runtime_error("JsonRpcClient: Failed to parse response JSON: " + std::string(e.what()));
    }

    if (!response.is_object()) {
        throw std::runtime_error("JsonRpcClient: Invalid response frame (not an object)");
    }

    // 校验 id 是否匹配
    if (response.contains("id") && !response["id"].is_null()) {
        uint64_t respId = response["id"].get<uint64_t>();
        if (respId != seq) {
            throw std::runtime_error("JsonRpcClient: Response ID mismatch");
        }
    }

    // 校验错误状态
    if (response.contains("error") && !response["error"].is_null()) {
        nlohmann::json err = response["error"];
        std::string errMsg = err.contains("message") ? err["message"].get<std::string>() : "Unknown error";
        throw std::runtime_error("JSON-RPC Server Error: " + errMsg);
    }

    if (!response.contains("result")) {
        throw std::runtime_error("JsonRpcClient: Response missing 'result' payload");
    }

    return response["result"];
}

} // namespace rpc
} // namespace tudou
