// ============================================================================
// 单连接 HTTP 请求解析上下文，持有 llhttp 状态机和正在构建的 HttpRequest。
// 将分片回调收敛为完整请求；不负责读取 TCP 数据、路由或发送响应。
// ============================================================================

#pragma once
#include <string>
#include "tudou/http/HttpRequest.h"
#include "llhttp.h"

class HttpContext {
public:
    enum class ParseResult {
        Rejected,
        NeedMoreData,
        Complete
    };

    HttpContext();
    ~HttpContext() = default;

    // 禁止拷贝和移动。llhttp 内部保存了指向当前实例的裸指针，复制或移动会破坏契约。
    HttpContext(const HttpContext&) = delete;
    HttpContext& operator=(const HttpContext&) = delete;
    HttpContext(HttpContext&&) = delete;
    HttpContext& operator=(HttpContext&&) = delete;

    ParseResult parse(const char* data, size_t len);
    HttpRequest& get_request() { return request_; }
    size_t get_consumed_bytes() const { return consumedBytes_; }
    void reset(); // 丢弃当前请求并让 llhttp 为下一条请求重新就绪。

private:
    static int on_message_begin(llhttp_t* parser);
    static int on_url(llhttp_t* parser, const char* at, size_t length);
    static int on_version_complete(llhttp_t* parser);
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);
    static int on_headers_complete(llhttp_t* parser);
    static int on_body(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete(llhttp_t* parser);

    static HttpContext* get_context(llhttp_t* parser) {
        return static_cast<HttpContext*>(parser->data);
    }

    void commit_pending_header(); // 将已收齐的 field/value 写入请求。
    void reset_message_state();

private:
    llhttp_t parser_;                   // llhttp 解析器实例，持有当前协议状态机。
    llhttp_settings_t settings_;        // llhttp 回调配置，绑定到当前上下文。

    HttpRequest request_;               // 当前正在构建的 HTTP 请求对象。
    size_t consumedBytes_;              // 单次 parse 中已消费的字节数。

    std::string currentUrl_;            // 当前请求目标缓存，解决 request line 跨片段时的覆盖问题。

    std::string pendingHeaderField_;    // 当前尚未提交的 Header Field 片段缓存。
    std::string pendingHeaderValue_;    // 当前尚未提交的 Header Value 片段缓存。
    bool hasPendingHeaderValue_;        // 当前 Header 是否已经接收到 value 回调。
};
