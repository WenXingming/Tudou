// ============================================================================
// HTTP 响应消息，保存状态行、Header、Body 并生成完整报文。
// 由 Router/Handler 构建，HttpServer 经 TCP/TLS 发送；不管理套接字或 TLS。
// ============================================================================

#include "tudou/http/HttpResponse.h"
#include "HttpResponse.h"

namespace {

constexpr char kContentLengthHeader[] = "Content-Length";

} // namespace

HttpResponse::HttpResponse() :
    httpVersion_("HTTP/1.1"),
    statusCode_(200),
    statusMessage_("OK"),
    headers_(),
    body_() {
}

std::string HttpResponse::serialize_to_string() const {
    // serialize_to_string 是响应对象的唯一出口，负责把字段状态转换成完整协议报文。
    std::string result;
    result.reserve(128 + body_.size());

    append_status_line(result);
    append_headers(result);
    append_body(result);
    return result;
}

void HttpResponse::set_status(int _code, const std::string& _message) {
    statusCode_ = _code;
    statusMessage_ = _message;
}

void HttpResponse::set_header(const std::string& field, const std::string& value) {
    // 同名响应头以后写入值为准，保持 DTO 的覆盖语义稳定。
    headers_[field] = value;
}

void HttpResponse::set_body(const std::string& body) {
    body_ = body;
}

bool HttpResponse::has_header(const std::string& field) const {
    return headers_.find(field) != headers_.end();
}

void HttpResponse::append_status_line(std::string& output) const {
    // 状态行必须位于报文最前面，后续头部和 body 都依赖这一行建立协议语境。
    output.append(httpVersion_);
    output.push_back(' ');
    output.append(std::to_string(statusCode_));
    output.push_back(' ');
    output.append(statusMessage_);
    output.append("\r\n");
}

void HttpResponse::append_headers(std::string& output) const {
    for (const auto& kv : headers_) {
        output.append(kv.first);
        output.push_back(':');
        output.push_back(' ');
        output.append(kv.second);
        output.append("\r\n");
    }

    if (!has_header(kContentLengthHeader)) {
        output.append(kContentLengthHeader);
        output.append(": ");
        output.append(std::to_string(body_.size()));
        output.append("\r\n");
    }
}

void HttpResponse::append_body(std::string& output) const {
    // 头部区和 body 之间的空行是 HTTP 报文边界的一部分，不能省略。
    output.append("\r\n");
    output.append(body_);
}
