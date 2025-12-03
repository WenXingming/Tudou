/**
 * @file HttpRequest.h
 * @brief HTTP 请求报文封装类
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou.git
 *
 * - 封装 HTTP 请求报文的各个部分，包括：
 *   - 方法
 *   - URL
 *   - 版本号
 *   - 头部字段
 *   - 消息体
 *   - path 和 query 是 URL 的拆分部分。例如，URL 为 xxx.com/path?query=1，则 path 为 xxx.com/path，query 为 query=1。
 * - 提供方法设置和获取这些部分: getters 和 setters。
 * - 提供清空请求报文内容的方法: clear()。
 * - HttpRequest 无需有 package_to_string() 方法，因为请求报文通常由客户端生成并发送。服务器端主要负责解析和处理请求报文，然后使用HttpResponse 用于生成响应报文。
 */

#pragma once

#include <string>
#include <unordered_map>


class HttpRequest {
public:
    using Headers = std::unordered_map<std::string, std::string>;

    void set_method(const std::string& m) { method = m; }
    const std::string& get_method() const { return method; }

    void set_url(const std::string& u) { url = u; }
    const std::string& get_url() const { return url; }

    void set_path(const std::string& p) { path = p; }
    const std::string& get_path() const { return path; }

    void set_query(const std::string& q) { query = q; }
    const std::string& get_query() const { return query; }

    void set_version(const std::string& v) { version = v; }
    const std::string& get_version() const { return version; }

    void add_header(const std::string& field, const std::string& value);
    const Headers& get_headers() const { return headers; }
    const std::string& get_header(const std::string& field) const;

    void append_body(const char* data, size_t len) { body.append(data, len); }
    void set_body(const std::string& b) { body = b; }
    const std::string& get_body() const { return body; }

    void clear();

private:
    std::string method;
    std::string url;
    std::string path;
    std::string query;
    std::string version;
    Headers headers;
    std::string body;
};

