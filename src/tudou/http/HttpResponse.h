/**
 * @file HttpResponse.h
 * @brief HTTP 响应报文封装类
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou
 *
 * - 封装 HTTP 响应报文的各个部分，包括：
 *   - 版本号
 *   - 状态码
 *   - 状态消息
 *   - 头部字段
 *   - 消息体
 * - 提供方法设置和获取这些部分: getters 和 setters。
 * - 提供将响应报文封装成字符串的方法: package_to_string()，便于通过网络发送。
 */

#pragma once
#include <string>
#include <unordered_map>

class HttpResponse {
public:
    using Headers = std::unordered_map<std::string, std::string>;

public:
    HttpResponse();
    ~HttpResponse() = default;

    std::string package_to_string() const;

    void set_http_version(const std::string& version) { httpVersion_ = version; }
    const std::string& get_http_version() const { return httpVersion_; }

    void set_status(int _code, const std::string& _message) {
        statusCode_ = _code;
        statusMessage_ = _message;
    }

    int get_status_code() const { return statusCode_; }
    const std::string& get_status_message() const { return statusMessage_; }

    void add_header(const std::string& field, const std::string& value) {
        headers_[field] = value;
    }
    const Headers& get_headers() const { return headers_; }
    void set_body(const std::string& _body) { body_ = _body; }
    const std::string& get_body() const { return body_; }

    void set_close_connection(bool _on) { closeConnection_ = _on; }
    bool get_close_connection() const { return closeConnection_; }

private:
    std::string httpVersion_;
    int statusCode_;
    std::string statusMessage_;
    Headers headers_;
    std::string body_;
    bool closeConnection_; // 标记连接是否需要关闭, 暂时未使用
};

