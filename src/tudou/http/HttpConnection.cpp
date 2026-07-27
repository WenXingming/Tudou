// ============================================================================
// HTTP 单连接状态实现：在网络字节、HTTP 请求和 HTTP 响应之间转换。
// 不读取或发送 socket，网络 I/O 始终由 TcpConnection 和 HttpServer 协作完成。
// ============================================================================

#include "tudou/http/HttpConnection.h"

#include <utility>

HttpConnection::HttpConnection(std::unique_ptr<TlsConnection> tlsConnection)
    : httpContext_()
    , tlsConnection_(std::move(tlsConnection)) {
}

HttpConnection::ProcessResult HttpConnection::decode_requests(const std::string& networkData, std::vector<HttpRequest>& requests, std::string& outboundCiphertext) {
    // 本轮只输出由当前网络数据组成的完整请求；未完成部分保留在 httpContext_ 中。
    requests.clear();

    // HTTPS 在此解密并可能生成握手回包；HTTP 明文连接直接透传。
    std::string plaintext;
    TlsConnection::ReadResult readResult = TlsConnection::ReadResult::Ready;
    if (tlsConnection_) {
        // TLS 会话同时维护握手状态；本次读操作可能需要产生反向发送的握手密文。
        readResult = tlsConnection_->read_plaintext(networkData, plaintext, outboundCiphertext);
    }
    else {
        // 明文 HTTP 无需协议转换，网络字节本身就是待解析的 HTTP 数据。
        plaintext = networkData;
        outboundCiphertext.clear();
    }

    if (readResult == TlsConnection::ReadResult::Error) {
        return ProcessResult::TlsError;
    }
    if (readResult != TlsConnection::ReadResult::Ready) {
        // 握手尚未完成或请求被拆包，当前没有可交给路由器的完整 HTTP 数据。
        return ProcessResult::Success;
    }

    // llhttp 每完成一条请求便暂停一次；循环因此能按到达顺序取出同一批数据中的 pipeline 请求。
    size_t consumed = 0;
    while (consumed < plaintext.size()) {
        const HttpContext::ParseResult parseResult =
            httpContext_.parse(plaintext.data() + consumed, plaintext.size() - consumed);
        const size_t lastConsumed = httpContext_.get_consumed_bytes();
        consumed += lastConsumed;

        if (parseResult == HttpContext::ParseResult::Rejected) {
            httpContext_.reset();
            return ProcessResult::BadRequest;
        }
        if (parseResult == HttpContext::ParseResult::Complete) {
            requests.push_back(std::move(httpContext_.get_request()));
            httpContext_.reset();
            continue;
        }
        if (lastConsumed == 0) {
            break;
        }
    }

    return ProcessResult::Success;
}

bool HttpConnection::encode_response(const HttpResponse& response, std::string& networkData) {
    std::string plaintext = response.serialize_to_string();

    if (tlsConnection_) {
        return tlsConnection_->write_plaintext(plaintext, networkData);
    }

    networkData = std::move(plaintext);
    return true;
}
