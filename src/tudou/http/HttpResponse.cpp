// ============================================================================
// HttpResponse.cpp
// HTTP 响应 DTO 实现，线性执行“状态行 -> 头部 -> 空行 -> body”序列化。
// ============================================================================

#include "HttpResponse.h"

namespace {

constexpr char kConnectionHeader[] = "Connection";
constexpr char kCloseConnectionValue[] = "close";
constexpr char kHttpVersion[] = "HTTP/1.1";
constexpr char kContentTypeHeader[] = "Content-Type";
constexpr char kContentLengthHeader[] = "Content-Length";
constexpr char kPlainTextContentType[] = "text/plain";

} // namespace

HttpResponse::HttpResponse() :
    httpVersion_("HTTP/1.1"),
    statusCode_(200),
    statusMessage_("OK"),
    headers_(),
    body_(),
    closeConnection_(false) {

}

HttpResponse HttpResponse::plain_text(int statusCode,
    const std::string& statusMessage,
    const std::string& body) {
    HttpResponse response;
    response.set_http_version(kHttpVersion);
    response.set_status(statusCode, statusMessage);
    response.set_body(body);
    response.set_header(kContentTypeHeader, kPlainTextContentType);
    response.set_header(kContentLengthHeader, std::to_string(body.size()));
    response.set_close_connection(true);
    return response;
}

void HttpResponse::set_header(const std::string& field, const std::string& value) {
    // 同名响应头以后写入值为准，保持 DTO 的覆盖语义稳定。
    headers_[field] = value;
}

bool HttpResponse::has_header(const std::string& field) const {
    return headers_.find(field) != headers_.end();
}

std::string HttpResponse::package_to_string() const {
    // package_to_string 是响应 DTO 的唯一出口，负责把字段状态转换成完整协议报文。
    std::string result;
    result.reserve(128 + body_.size());

    append_status_line(result);
    append_headers(result);
    append_body(result);
    return result;
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

    // closeConnection_ 是显式协议意图，不应该在序列化时悄悄丢失。
    if (closeConnection_ && !has_header(kConnectionHeader)) {
        output.append(kConnectionHeader);
        output.append(": ");
        output.append(kCloseConnectionValue);
        output.append("\r\n");
    }
}

void HttpResponse::append_body(std::string& output) const {
    // 头部区和 body 之间的空行是 HTTP 报文边界的一部分，不能省略。
    output.append("\r\n");
    output.append(body_);
}

