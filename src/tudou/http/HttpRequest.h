// ============================================================================
// HTTP 请求 DTO，保存解析后的请求行、Header 与 Body。
// 由 HttpContext 构建，供 Router 和业务 Handler 读取；不参与解析或路由。
// ============================================================================

#pragma once
#include <string>
#include <unordered_map>

class HttpRequest {
public:
    using Headers = std::unordered_map<std::string, std::string>;

    HttpRequest();
    ~HttpRequest() = default;

    void set_method(const std::string& m) { method_ = m; }
    void set_url(const std::string& u) { url_ = u; }
    void set_path(const std::string& p) { path_ = p; }
    void set_query(const std::string& q) { query_ = q; }
    void set_version(const std::string& v) { version_ = v; }
    void add_header(const std::string& field, const std::string& value); // 写入或覆盖一个请求头。
    void append_body(const char* data, size_t len) { body_.append(data, len); }
    void set_body(const std::string& b) { body_ = b; }

    const std::string& get_method() const { return method_; }
    const std::string& get_url() const { return url_; }
    const std::string& get_path() const { return path_; }
    const std::string& get_query() const { return query_; }
    const std::string& get_version() const { return version_; }
    const Headers& get_headers() const { return headers_; }
    const std::string& get_header(const std::string& field) const;
    const std::string& get_body() const { return body_; }

    void clear();

private:
    std::string method_;                // 请求方法。
    std::string url_;                   // 原始请求目标。
    std::string path_;                  // 解析后的 path。
    std::string query_;                 // 解析后的 query。
    std::string version_;               // HTTP 版本。
    Headers headers_;                   // 请求头集合。
    std::string body_;                  // 请求体。
};
