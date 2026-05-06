// ============================================== //
// HttpRequest.h
// HTTP 请求 DTO，负责承载解析后的协议字段，不参与业务编排。
// ============================================== //

#pragma once
#include <string>
#include <unordered_map>

// HttpRequest 是解析层输出的稳定契约对象，只保存协议字段，不包含任何流程控制。
class HttpRequest {
    using Headers = std::unordered_map<std::string, std::string>;

public:
    HttpRequest();
    ~HttpRequest() = default;

    /**
     * @brief 设置请求方法。
     * @param m HTTP Method。
     */
    void set_method(const std::string& m) { method_ = m; }

    /**
     * @brief 获取请求方法。
     * @return 当前请求方法。
     */
    const std::string& get_method() const { return method_; }

    /**
     * @brief 设置原始请求目标。
     * @param u 原始 URL。
     */
    void set_url(const std::string& u) { url_ = u; }

    /**
     * @brief 获取原始请求目标。
     * @return 当前请求 URL。
     */
    const std::string& get_url() const { return url_; }

    /**
     * @brief 设置请求路径。
     * @param p 解析后的 path。
     */
    void set_path(const std::string& p) { path_ = p; }

    /**
     * @brief 获取请求路径。
     * @return 当前请求 path。
     */
    const std::string& get_path() const { return path_; }

    /**
     * @brief 设置查询字符串。
     * @param q 解析后的 query。
     */
    void set_query(const std::string& q) { query_ = q; }

    /**
     * @brief 获取查询字符串。
     * @return 当前请求 query。
     */
    const std::string& get_query() const { return query_; }

    /**
     * @brief 设置协议版本。
     * @param v HTTP 版本字符串。
     */
    void set_version(const std::string& v) { version_ = v; }

    /**
     * @brief 获取协议版本。
     * @return 当前请求 HTTP 版本。
     */
    const std::string& get_version() const { return version_; }

    /**
     * @brief 写入或覆盖一个请求头。
     * @param field 请求头名称。
     * @param value 请求头值。
     */
    void add_header(const std::string& field, const std::string& value);

    /**
     * @brief 获取全部请求头。
     * @return 当前请求头映射的只读引用。
     */
    const Headers& get_headers() const { return headers_; }

    /**
     * @brief 按名称获取请求头。
     * @param field 请求头名称。
     * @return 若存在则返回对应值，否则返回空字符串引用。
     */
    const std::string& get_header(const std::string& field) const;

    /**
     * @brief 追加请求体分片。
     * @param data 请求体片段起始地址。
     * @param len 请求体片段长度。
     */
    void append_body(const char* data, size_t len) { body_.append(data, len); }

    /**
     * @brief 直接设置完整请求体。
     * @param b 完整请求体。
     */
    void set_body(const std::string& b) { body_ = b; }

    /**
     * @brief 获取请求体。
     * @return 当前请求体内容。
     */
    const std::string& get_body() const { return body_; }

    /**
     * @brief 清空所有协议字段，回到初始状态。
     */
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

