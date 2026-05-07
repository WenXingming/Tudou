// ============================================================================
// HttpRequest.h
// HTTP 请求 DTO，负责承载解析后的协议字段，不参与业务编排。
//
// 成员函数调用树（[公有] 标注接口层级）：
//
// HttpRequest.h
// └── HttpRequest
//     ├── HttpRequest()                          # [公有] 构造一个空请求 DTO
//     ├── ~HttpRequest()                         # [公有] 默认析构
//     ├── set_method(m)                          # [公有] 写入请求方法
//     ├── get_method() const                     # [公有] 读取请求方法
//     ├── set_url(u)                             # [公有] 写入原始请求目标
//     ├── get_url() const                        # [公有] 读取原始请求目标
//     ├── set_path(p)                            # [公有] 写入解析后的 path
//     ├── get_path() const                       # [公有] 读取 path
//     ├── set_query(q)                           # [公有] 写入解析后的 query
//     ├── get_query() const                      # [公有] 读取 query
//     ├── set_version(v)                         # [公有] 写入 HTTP 版本
//     ├── get_version() const                    # [公有] 读取 HTTP 版本
//     ├── add_header(field, value)               # [公有] 写入或覆盖一个请求头
//     ├── get_headers() const                    # [公有] 读取全部请求头
//     ├── get_header(field) const                # [公有] 按名称读取请求头
//     ├── append_body(data, len)                 # [公有] 追加请求体分片
//     ├── set_body(b)                            # [公有] 直接设置完整请求体
//     ├── get_body() const                       # [公有] 读取请求体
//     └── clear()                                # [公有] 清空全部协议字段
// ============================================================================

#pragma once
#include <string>
#include <unordered_map>

// HttpRequest 是解析层输出的稳定契约对象，只保存协议字段，不包含任何流程控制。
class HttpRequest {
    using Headers = std::unordered_map<std::string, std::string>;

public:
    HttpRequest();
    ~HttpRequest() = default;

    void set_method(const std::string& m) { method_ = m; }
    const std::string& get_method() const { return method_; }
    void set_url(const std::string& u) { url_ = u; }
    const std::string& get_url() const { return url_; }
    void set_path(const std::string& p) { path_ = p; }
    const std::string& get_path() const { return path_; }
    void set_query(const std::string& q) { query_ = q; }
    const std::string& get_query() const { return query_; }
    void set_version(const std::string& v) { version_ = v; }
    const std::string& get_version() const { return version_; }
    void add_header(const std::string& field, const std::string& value); // 写入或覆盖一个请求头。

    const Headers& get_headers() const { return headers_; }
    const std::string& get_header(const std::string& field) const;
    void append_body(const char* data, size_t len) { body_.append(data, len); }
    void set_body(const std::string& b) { body_ = b; }
    const std::string& get_body() const { return body_; }
    void clear();

private:
    std::string method_;   // 请求方法。
    std::string url_;      // 原始请求目标。
    std::string path_;     // 解析后的 path。
    std::string query_;    // 解析后的 query。
    std::string version_;  // HTTP 版本。
    Headers headers_;      // 请求头集合。
    std::string body_;     // 请求体。
};

