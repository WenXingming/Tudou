// ============================================================================
// HttpContext.h
// HTTP 请求解析上下文，负责管理单连接 HTTP 解析状态，把 llhttp 的回调流收敛成稳定的 HttpRequest。以 parse() 作为唯一对外解析门面。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// HttpContext.h
// └── HttpContext
//     ├── HttpContext()                          # [公有] 初始化 llhttp 配置并绑定静态回调桥
//     ├── HttpContext(copy)                      # [公有] 删除拷贝构造，保持 parser_.data 指针稳定
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值，避免复制解析状态机
//     ├── HttpContext(move)                      # [公有] 删除移动构造，避免 llhttp 持有悬空指针
//     ├── operator=(move)                        # [公有] 删除移动赋值，保持上下文地址稳定
//     ├── ~HttpContext()                         # [公有] 默认析构
//     ├── parse(data, len)                       # [公有] 解析总入口，返回 Reject/NeedMoreData/Complete
//     │   ├── on_message_begin(parser)           # [私有] 静态桥：消息开始回调
//     │   │   ├── get_context(parser)            # [私有] 恢复当前 HttpContext
//     │   │   └── on_message_begin_impl()        # [私有] 重置本条消息的构建状态
//     │   │       └── reset_message_state()      # [私有] 清空当前消息的全部中间状态
//     │   ├── on_url(parser, at, length)         # [私有] 静态桥：URL 片段回调
//     │   │   ├── get_context(parser)            # [私有] 恢复当前 HttpContext
//     │   │   └── on_url_impl(at, length)        # [私有] 累计 request target 并记录 method
//     │   │       ├── append_request_target_fragment(...)  # [私有] 追加 URL 片段
//     │   │       └── capture_request_method()   # [私有] 从 llhttp 状态提取 method
//     │   ├── on_header_field(parser, at, length)  # [私有] 静态桥：Header Field 回调
//     │   │   ├── get_context(parser)            # [私有] 恢复当前 HttpContext
//     │   │   └── on_header_field_impl(at, length)  # [私有] 必要时先提交上一个 header，再累计 field
//     │   │       └── flush_pending_header()     # [私有] 提交已闭合的 header 键值
//     │   ├── on_header_value(parser, at, length)  # [私有] 静态桥：Header Value 回调
//     │   │   ├── get_context(parser)            # [私有] 恢复当前 HttpContext
//     │   │   └── on_header_value_impl(at, length)  # [私有] 累计 value 片段
//     │   ├── on_body(parser, at, length)        # [私有] 静态桥：Body 片段回调
//     │   │   ├── get_context(parser)            # [私有] 恢复当前 HttpContext
//     │   │   └── on_body_impl(at, length)       # [私有] 把 body 片段追加到请求体
//     │   └── on_message_complete(parser)        # [私有] 静态桥：消息结束回调
//     │       ├── get_context(parser)            # [私有] 恢复当前 HttpContext
//     │       └── on_message_complete_impl()     # [私有] 提交尾 header、补齐 target/version，并标记完成
//     │           ├── flush_pending_header()     # [私有] 提交最后一个 header
//     │           ├── apply_request_target()     # [私有] 拆分 path 与 query
//     │           └── capture_http_version()     # [私有] 根据解析器状态写入 HTTP 版本
//     ├── get_request() const                    # [公有] 读取当前解析出的 HttpRequest
//     └── reset()                                # [公有] 重置请求状态和 llhttp 解析器
//         └── reset_message_state()              # [私有] 清空当前消息的全部中间状态
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

    ParseResult parse(const char* data, size_t len); // 执行一次 llhttp 解析并返回当前解析状态。

    const HttpRequest& get_request() const { return request_; }
    void reset();

private:
    static int on_message_begin(llhttp_t* parser);
    static int on_url(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);
    static int on_body(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete(llhttp_t* parser);
    static HttpContext* get_context(llhttp_t* parser) {
        return static_cast<HttpContext*>(parser->data);
    }

    void reset_message_state();
    void flush_pending_header(); // 提交已经闭合的一组 header 键值。

private:
    llhttp_t parser_;                   // llhttp 解析器实例，持有当前协议状态机。
    llhttp_settings_t settings_;        // llhttp 回调配置，绑定到当前上下文。

    HttpRequest request_;               // 当前正在构建的 HTTP 请求对象。

    bool messageComplete_;              // 当前请求是否已经完成解析。
    std::string currentUrl_;            // 当前请求目标缓存，解决 request line 跨片段时的覆盖问题。

    std::string currentHeaderField_;    // 当前尚未提交的 Header Field 片段缓存。
    std::string currentHeaderValue_;    // 当前尚未提交的 Header Value 片段缓存。
    bool lastWasValue_;                 // 标记最近一次回调是否为 Header Value，用于识别一个头部是否闭合。
};
