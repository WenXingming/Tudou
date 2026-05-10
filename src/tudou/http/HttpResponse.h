// ============================================================================
// HttpResponse.h
// HTTP 响应 DTO，负责持有协议字段并序列化为可发送报文。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// HttpResponse.h
// └── HttpResponse
//     ├── HttpResponse()                         # [公有] 构造默认 200 OK 响应 DTO
//     ├── ~HttpResponse()                        # [公有] 默认析构
//     ├── package_to_string() const              # [公有] 序列化总入口：状态行 -> 头部 -> 空行 -> body
//     │   ├── append_status_line(output) const   # [私有] 追加状态行
//     │   ├── append_headers(output) const       # [私有] 追加响应头并按需补 Connection: close
//     │   └── append_body(output) const          # [私有] 追加空行和响应体
//     ├── set_http_version(version)              # [公有] 写入 HTTP 版本
//     ├── get_http_version() const               # [公有] 读取 HTTP 版本
//     ├── set_status(code, message)              # [公有] 写入状态码和状态描述
//     ├── get_status_code() const                # [公有] 读取状态码
//     ├── get_status_message() const             # [公有] 读取状态描述
//     ├── set_header(field, value)               # [公有] 写入或覆盖一个响应头
//     ├── has_header(field) const                # [公有] 判断响应头是否存在
//     ├── get_headers() const                    # [公有] 读取全部响应头
//     ├── set_body(body)                         # [公有] 写入响应体
//     ├── get_body() const                       # [公有] 读取响应体
//     ├── set_close_connection(on)               # [公有] 标记响应后是否关闭连接
//     └── get_close_connection() const           # [公有] 读取关闭连接标记
// ============================================================================

#pragma once
#include <string>
#include <unordered_map>

// HttpResponse 只负责表达协议结果，不参与底层发送流程。
class HttpResponse {
public:
    using Headers = std::unordered_map<std::string, std::string>;

public:
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
    void set_body(const std::string& _body) { body_ = _body; }
    const std::string& get_body() const { return body_; }
    void set_close_connection(bool _on) { closeConnection_ = _on; }
    bool get_close_connection() const { return closeConnection_; }

private:
    void append_status_line(std::string& output) const;
    void append_headers(std::string& output) const; // 追加响应头，并按需补齐 close 语义。
    void append_body(std::string& output) const;

private:
    std::string httpVersion_;     // 响应行中的 HTTP 版本。
    int statusCode_;              // 响应状态码。
    std::string statusMessage_;   // 响应状态描述。
    Headers headers_;             // 响应头集合。
    std::string body_;            // 响应体。
    bool closeConnection_;        // 标记响应后连接是否应关闭。
};

