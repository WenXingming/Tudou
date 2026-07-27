// ============================================================================
// HTTP 层单连接状态：提取 HTTP 请求（网络字节 → TLS 明文 → 完整请求）并将 HTTP 响应转换为网络字节（响应 → 网络字节）。
// 与 TcpConnection 一一关联，但不拥有 fd、Socket 或底层 I/O 生命周期。
// ============================================================================

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "tudou/http/HttpContext.h"
#include "tudou/http/HttpResponse.h"
#include "tudou/http/TlsConnection.h"

class HttpConnection {
public:
    enum class ProcessResult {
        Success,      // 本轮网络数据已处理；可能尚未组成完整请求。
        BadRequest,   // HTTP 语法错误。
        TlsError      // TLS 会话不可继续使用。
    };

    explicit HttpConnection(std::unique_ptr<TlsConnection> tlsConnection = nullptr);

    // 处理网络字节，按出现顺序输出完整请求和 TLS 握手待发密文。
    ProcessResult decode_requests(const std::string& networkData, std::vector<HttpRequest>& requests, std::string& outboundCiphertext);

    // 把 HttpResponse 编码成网络字节：序列化 HTTP 响应并加密（如有 TLS）为可直接发送的网络字节。
    bool encode_response(const HttpResponse& response, std::string& networkData);

private:
    HttpContext httpContext_;                      // 当前 HTTP 请求的增量解析状态。
    std::unique_ptr<TlsConnection> tlsConnection_; // HTTPS 连接独有的 TLS 状态。可为 nullptr，表示 HTTP 明文连接。
};
