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

    HttpResponse() = default;

    void set_status(int _code, const std::string& _message) {
        statusCode = _code;
        statusMessage = _message;
    }

    int get_status_code() const { return statusCode; }
    const std::string& get_status_message() const { return statusMessage; }

    void set_close_connection(bool _on) { closeConnection = _on; }
    bool get_close_connection() const { return closeConnection; }

    void set_body(const std::string& _body) { body = _body; }
    const std::string& get_body() const { return body; }

    void add_header(const std::string& field, const std::string& value) {
        headers[field] = value;
    }
    const Headers& get_headers() const { return headers; }

    std::string package_to_string() const;

private:
    std::string httpVersion = "HTTP/1.1";
    int statusCode{ 200 };
    std::string statusMessage = "OK";
    bool closeConnection{ false };
    Headers headers;
    std::string body;
};

