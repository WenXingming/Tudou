// ============================================== //
// HttpResponse.h
// HTTP 响应 DTO，负责持有协议字段并序列化为可发送报文。
// ============================================== //

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

    /**
     * @brief 将当前响应对象序列化为 HTTP 响应报文。
     * @return 可直接发送到网络层的完整响应字符串。
     */
    std::string package_to_string() const;

    /**
     * @brief 设置 HTTP 版本。
     * @param version 响应行中的协议版本。
     */
    void set_http_version(const std::string& version) { httpVersion_ = version; }

    /**
     * @brief 获取 HTTP 版本。
     * @return 当前响应行中的协议版本。
     */
    const std::string& get_http_version() const { return httpVersion_; }

    /**
     * @brief 设置状态码与状态消息。
     * @param code HTTP 状态码。
     * @param message HTTP 状态描述。
     */
    void set_status(int _code, const std::string& _message) {
        statusCode_ = _code;
        statusMessage_ = _message;
    }

    /**
     * @brief 获取状态码。
     * @return 当前响应状态码。
     */
    int get_status_code() const { return statusCode_; }

    /**
     * @brief 获取状态消息。
     * @return 当前响应状态消息。
     */
    const std::string& get_status_message() const { return statusMessage_; }

    /**
     * @brief 写入或覆盖一个响应头。
     * @param field 响应头名称。
     * @param value 响应头值。
     */
    void set_header(const std::string& field, const std::string& value);

    /**
     * @brief 兼容旧接口，写入或覆盖一个响应头。
     * @param field 响应头名称。
     * @param value 响应头值。
     */
    void add_header(const std::string& field, const std::string& value) {
        set_header(field, value);
    }

    /**
     * @brief 判断某个响应头是否已经存在。
     * @param field 响应头名称。
     * @return true 表示响应头已存在。
     */
    bool has_header(const std::string& field) const;

    /**
     * @brief 获取全部响应头。
     * @return 当前响应头映射的只读引用。
     */
    const Headers& get_headers() const { return headers_; }

    /**
     * @brief 设置响应体。
     * @param body 响应体内容。
     */
    void set_body(const std::string& _body) { body_ = _body; }

    /**
     * @brief 获取响应体。
     * @return 当前响应体内容。
     */
    const std::string& get_body() const { return body_; }

    /**
     * @brief 标记响应发送后是否关闭连接。
     * @param on true 表示应关闭连接。
     */
    void set_close_connection(bool _on) { closeConnection_ = _on; }

    /**
     * @brief 查询响应是否要求关闭连接。
     * @return true 表示连接应在响应后关闭。
     */
    bool get_close_connection() const { return closeConnection_; }

private:
    /**
     * @brief 追加状态行。
     * @param output 目标响应字符串。
     */
    void append_status_line(std::string& output) const;

    /**
     * @brief 追加显式设置的响应头，并在需要时补齐 Connection: close。
     * @param output 目标响应字符串。
     */
    void append_headers(std::string& output) const;

    /**
     * @brief 追加响应体分隔符和 body。
     * @param output 目标响应字符串。
     */
    void append_body(std::string& output) const;

private:
    std::string httpVersion_;     // 响应行中的 HTTP 版本。
    int statusCode_;              // 响应状态码。
    std::string statusMessage_;   // 响应状态描述。
    Headers headers_;             // 响应头集合。
    std::string body_;            // 响应体。
    bool closeConnection_;        // 标记响应后连接是否应关闭。
};

