// ============================================== //
// HttpContext.h
// HTTP 请求解析上下文，负责把 llhttp 的回调流收敛成稳定的 HttpRequest。
// ============================================== //

#pragma once
#include <string>
#include "HttpRequest.h"
#include "llhttp/llhttp.h"
// 负责管理单连接 HTTP 解析状态，并以 parse() 作为唯一对外解析门面。
class HttpContext {
public:
    HttpContext();
    ~HttpContext() = default;

    // 禁止拷贝和移动。llhttp 内部保存了指向当前实例的裸指针，复制或移动会破坏契约。
    HttpContext(const HttpContext&) = delete;
    HttpContext& operator=(const HttpContext&) = delete;
    HttpContext(HttpContext&&) = delete;
    HttpContext& operator=(HttpContext&&) = delete;

    /**
     * @brief 执行一次 HTTP 请求解析。
     * @param data 输入缓冲区。
     * @param len 输入缓冲区长度。
     * @param nparsed 输出本次被解析器接受的字节数。
     * @return true 表示 llhttp 接受了本批输入；false 表示出现协议解析错误。
     */
    bool parse(const char* data, size_t len, size_t& nparsed);

    /**
     * @brief 判断当前请求是否已经完整结束。
     * @return true 表示已经收到完整 HTTP 请求。
     */
    bool is_complete() const { return messageComplete_; }

    /**
     * @brief 获取当前已解析出的请求对象。
     * @return 当前上下文持有的 HttpRequest 只读引用。
     */
    const HttpRequest& get_request() const { return request_; }

    /**
     * @brief 清空请求与解析状态，为下一条报文做准备。
     */
    void reset();

private:
    /**
     * @brief 处理 llhttp 的 message begin 事件。
     * @param parser llhttp 解析器实例。
     * @return 总是返回 0，表示继续解析。
     */
    static int on_message_begin(llhttp_t* parser);

    /**
     * @brief 处理 llhttp 的 URL 片段回调。
     * @param parser llhttp 解析器实例。
     * @param at URL 数据起始地址。
     * @param length URL 数据长度。
     * @return 总是返回 0，表示继续解析。
     */
    static int on_url(llhttp_t* parser, const char* at, size_t length);

    /**
     * @brief 处理 llhttp 的 Header Field 片段回调。
     * @param parser llhttp 解析器实例。
     * @param at Header Field 数据起始地址。
     * @param length Header Field 数据长度。
     * @return 总是返回 0，表示继续解析。
     */
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);

    /**
     * @brief 处理 llhttp 的 Header Value 片段回调。
     * @param parser llhttp 解析器实例。
     * @param at Header Value 数据起始地址。
     * @param length Header Value 数据长度。
     * @return 总是返回 0，表示继续解析。
     */
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);

    /**
     * @brief 处理 llhttp 的 Body 片段回调。
     * @param parser llhttp 解析器实例。
     * @param at Body 数据起始地址。
     * @param length Body 数据长度。
     * @return 总是返回 0，表示继续解析。
     */
    static int on_body(llhttp_t* parser, const char* at, size_t length);

    /**
     * @brief 处理 llhttp 的 message complete 事件。
     * @param parser llhttp 解析器实例。
     * @return 总是返回 0，表示继续解析。
     */
    static int on_message_complete(llhttp_t* parser);

    /**
     * @brief 把 llhttp 的用户数据恢复成当前上下文实例。
     * @param parser llhttp 解析器实例。
     * @return 当前 HttpContext 指针。
     */
    static HttpContext* get_context(llhttp_t* parser) {
        return static_cast<HttpContext*>(parser->data);
    }

    /**
     * @brief 处理一条新消息的开始事件。
     */
    void on_message_begin_impl();

    /**
     * @brief 处理 URL 片段并同步 method、path、query、version。
     * @param at URL 数据起始地址。
     * @param length URL 数据长度。
     */
    void on_url_impl(const char* at, size_t length);

    /**
     * @brief 处理 Header Field 片段。
     * @param at Header Field 数据起始地址。
     * @param length Header Field 数据长度。
     */
    void on_header_field_impl(const char* at, size_t length);

    /**
     * @brief 处理 Header Value 片段。
     * @param at Header Value 数据起始地址。
     * @param length Header Value 数据长度。
     */
    void on_header_value_impl(const char* at, size_t length);

    /**
     * @brief 处理 Body 片段并追加到请求体。
     * @param at Body 数据起始地址。
     * @param length Body 数据长度。
     */
    void on_body_impl(const char* at, size_t length);

    /**
     * @brief 收尾并标记当前消息解析完成。
     */
    void on_message_complete_impl();

    /**
     * @brief 清空请求构建阶段的所有中间状态。
     */
    void reset_message_state();

    /**
     * @brief 追加一段 URL 片段缓存，等待请求行完整后再统一落到 HttpRequest。
     * @param at URL 数据起始地址。
     * @param length URL 数据长度。
     */
    void append_request_target_fragment(const char* at, size_t length);

    /**
     * @brief 从 llhttp 解析器中提取当前 HTTP Method。
     */
    void capture_request_method();

    /**
     * @brief 应用完整 URL，并拆分 path 与 query。
     */
    void apply_request_target();

    /**
     * @brief 根据当前解析器状态设置 HTTP 版本。
     */
    void capture_http_version();

    /**
     * @brief 如果上一个头字段已经闭合，则将其提交到请求对象。
     */
    void flush_pending_header();

private:
    llhttp_t parser_;                       // llhttp 解析器实例，持有当前协议状态机。
    llhttp_settings_t settings_;            // llhttp 回调配置，绑定到当前上下文。

    HttpRequest request_;                   // 当前正在构建的 HTTP 请求对象。

    bool messageComplete_;                  // 当前请求是否已经完成解析。
    std::string currentUrl_;                // 当前请求目标缓存，解决 request line 跨片段时的覆盖问题。

    std::string currentHeaderField_;        // 当前尚未提交的 Header Field 片段缓存。
    std::string currentHeaderValue_;        // 当前尚未提交的 Header Value 片段缓存。
    bool lastWasValue_;                     // 标记最近一次回调是否为 Header Value，用于识别一个头部是否闭合。
};

