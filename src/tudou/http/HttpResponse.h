// ============================================================================
// HTTP 响应消息，保存状态行、Header、Body 并生成完整报文。
// 由 Router/Handler 构建，HttpServer 经 TCP/TLS 发送；不管理套接字或 TLS。
// ============================================================================

#pragma once
#include <string>
#include <unordered_map>

class HttpResponse {
public:
    using Headers = std::unordered_map<std::string, std::string>;

    HttpResponse();
    ~HttpResponse() = default;

    std::string serialize_to_string() const; // 将当前响应对象序列化为完整 HTTP 报文。

    void set_http_version(const std::string& version) { httpVersion_ = version; }
    void set_status(int _code, const std::string& _message);
    void set_header(const std::string& field, const std::string& value); // 写入或覆盖一个响应头。
    void set_body(const std::string& body);

    const std::string& get_http_version() const { return httpVersion_; }
    int get_status_code() const { return statusCode_; }
    const std::string& get_status_message() const { return statusMessage_; }
    bool has_header(const std::string& field) const;
    const Headers& get_headers() const { return headers_; }
    const std::string& get_body() const { return body_; }

private:
    void append_status_line(std::string& output) const;
    void append_headers(std::string& output) const; // 追加响应头，并按需补齐 Content-Length。
    void append_body(std::string& output) const;

private:
    std::string httpVersion_;           // 响应行中的 HTTP 版本。
    int statusCode_;                    // 响应状态码。
    std::string statusMessage_;         // 响应状态描述。
    Headers headers_;                   // 响应头集合。
    std::string body_;                  // 响应体。
};
