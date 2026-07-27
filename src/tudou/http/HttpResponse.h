// ============================================================================
// HTTP 响应 DTO，负责持有协议字段并序列化为可发送报文。
// ============================================================================

#pragma once
#include <string>
#include <unordered_map>

// HttpResponse 只负责表达协议结果，不参与底层发送流程。
class HttpResponse {
public:
    using Headers = std::unordered_map<std::string, std::string>;
    HttpResponse();
    ~HttpResponse() = default;

    static HttpResponse plain_text(int statusCode,
        const std::string& statusMessage,
        const std::string& body);

    std::string package_to_string() const; // 将当前响应对象序列化为完整 HTTP 报文。

    void set_http_version(const std::string& version) { httpVersion_ = version; }
    const std::string& get_http_version() const { return httpVersion_; }
    void set_status(int _code, const std::string& _message) {
        statusCode_ = _code;
        statusMessage_ = _message;
    }
    int get_status_code() const { return statusCode_; }
    const std::string& get_status_message() const { return statusMessage_; }
    void set_header(const std::string& field, const std::string& value); // 写入或覆盖一个响应头。

    bool has_header(const std::string& field) const;
    const Headers& get_headers() const { return headers_; }
    void set_body(const std::string& body);
    const std::string& get_body() const { return body_; }
    void set_close_connection(bool _on) { closeConnection_ = _on; }
    bool get_close_connection() const { return closeConnection_; }

private:
    void append_status_line(std::string& output) const;
    void append_headers(std::string& output) const; // 追加响应头，并按需补齐 close 语义。
    void append_body(std::string& output) const;

private:
    std::string httpVersion_;           // 响应行中的 HTTP 版本。
    int statusCode_;                    // 响应状态码。
    std::string statusMessage_;         // 响应状态描述。
    Headers headers_;                   // 响应头集合。
    std::string body_;                  // 响应体。
    bool closeConnection_;              // 标记响应后连接是否应关闭。
};
