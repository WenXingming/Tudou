// ============================================================================
// 基于 TCP 阻塞传输的 JSON-RPC 2.0 客户端实现。
// ============================================================================

#include "JsonRpcClient.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <stdexcept>

namespace tudou {
namespace rpc {

JsonRpcClient::JsonRpcClient(const std::string& ip, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("JsonRpcClient: Failed to create socket");
    }
    clientFd_.reset(fd);

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(port);

    if (::inet_pton(AF_INET, ip.c_str(), &servAddr.sin_addr) != 1) {
        throw std::runtime_error("JsonRpcClient: Invalid IP address: " + ip);
    }

    if (::connect(clientFd_.fd(), reinterpret_cast<struct sockaddr*>(&servAddr), sizeof(servAddr)) < 0) {
        throw std::runtime_error("JsonRpcClient: Failed to connect to server " + ip + ":" + std::to_string(port));
    }
}

JsonRpcClient::~JsonRpcClient() = default;

nlohmann::json JsonRpcClient::call(const std::string& method, const nlohmann::json& params) {
    uint64_t seq = nextSequenceId_++;
    std::string requestStr = encode_request(method, params, seq);
    send_all(requestStr);
    std::string responseLine = read_line();
    return decode_response(responseLine, seq);
}

std::string JsonRpcClient::encode_request(const std::string& method, const nlohmann::json& params, uint64_t seq) {
    nlohmann::json request;
    request["jsonrpc"] = "2.0";
    request["method"] = method;
    if (!params.is_null()) {
        request["params"] = params;
    }
    request["id"] = seq;
    return request.dump() + "\n";
}

void JsonRpcClient::send_all(const std::string& data) {
    size_t totalSent = 0;
    while (totalSent < data.size()) {
        ssize_t n = ::write(clientFd_.fd(), data.data() + totalSent, data.size() - totalSent);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("JsonRpcClient: Failed to send request bytes");
        }
        totalSent += n;
    }
}

std::string JsonRpcClient::read_line() {
    // 粘包：同步阻塞（Ping-Pong 一发一收）客户端，该模型下不存在多响应粘包
    // 拆包： while 循环只会在“数据还没拼完整（没找到  \n ）”时继续读，一旦拼完整（找到  \n ）或者网络出错/断开就会立刻退出，不会无限读
    char temp[512];
    size_t delimiterPos = recvBuf_.find('\n');
    while (delimiterPos == std::string::npos) {
        ssize_t nr = ::read(clientFd_.fd(), temp, sizeof(temp));
        if (nr <= 0) {
            if (nr < 0 && errno == EINTR) {
                continue;
            }
            throw std::runtime_error("JsonRpcClient: Connection closed by remote server while waiting for response");
        }
        recvBuf_.append(temp, nr);
        delimiterPos = recvBuf_.find('\n');
    }

    std::string line = recvBuf_.substr(0, delimiterPos);
    recvBuf_.erase(0, delimiterPos + 1);
    return line;
}

nlohmann::json JsonRpcClient::decode_response(const std::string& line, uint64_t seq) {
    nlohmann::json response;
    try {
        response = nlohmann::json::parse(line);
    }
    catch (const std::exception& e) {
        throw std::runtime_error("JsonRpcClient: Failed to parse response JSON: " + std::string(e.what()));
    }

    if (!response.is_object()) {
        throw std::runtime_error("JsonRpcClient: Invalid response frame (not an object)");
    }

    if (response.contains("id") && !response["id"].is_null()) {
        uint64_t respId = response["id"].get<uint64_t>();
        if (respId != seq) {
            throw std::runtime_error("JsonRpcClient: Response ID mismatch");
        }
    }

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
