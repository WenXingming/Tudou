/**
 * @file HttpContext.h
 * @brief HTTP 报文解析上下文封装类
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou.git
 *
 * - 解析 HTTP 报文的类，基于 llhttp 库实现。解析 HTTP 请求报文，结果存储在 HttpRequest 对象中。
 *
 * - 对外接口包括：
 *   - parse(): 解析传入的数据，返回是否成功解析。nparsed 输出实际解析的字节数。只要无错误即视为消费了全部输入数据即 len 字节
 *   - is_complete(): 检查报文是否完整解析，判断是否收到了完整的一个 HTTP 请求
 *   - get_request(): 解析完成后，通过此方法获取解析后的 HttpRequest 对象，包含请求的各个部分
 *   - reset(): 重置解析器状态，准备解析新的 HTTP 报文
 *
 * - 内部通过 llhttp_settings_t 设置回调函数，处理报文的各个部分，如 URL、头部字段、消息体等，逐步构建 HttpRequest 对象。
 *   - on_message_begin: 报文开始，重置 HttpRequest 对象
 *   - on_url: 解析 URL，拆分 path 和 query，设置方法和版本
 *   - on_header_field / on_header_value: 解析头部字段，存储到 HttpRequest 对象
 *   - on_body: 解析消息体，追加到 HttpRequest 对象
 *   - on_message_complete: 报文结束，标记解析完成
 */

#pragma once
#include <string>
#include "HttpRequest.h"
#include "llhttp/llhttp.h"


class HttpContext {
public:
    HttpContext();

    // 禁止拷贝和移动构造。因为 llhttp_t 内部有指针指向 HttpContext 实例，拷贝或移动会导致指针失效。
    HttpContext(const HttpContext&) = delete;
    HttpContext& operator=(const HttpContext&) = delete;
    HttpContext(HttpContext&&) = delete;
    HttpContext& operator=(HttpContext&&) = delete;

    // 关键函数：返回是否解析成功，nparsed 输出实际解析的字节数
    bool parse(const char* data, size_t len, size_t& nparsed);

    bool is_complete() const { return messageComplete; }

    const HttpRequest& get_request() const { return request; }

    void reset();

private:
    // 回调函数的参数是固定要求的，需要将 parser 指针转换回 HttpContext 实例指针
    static int on_message_begin(llhttp_t* parser);
    static int on_url(llhttp_t* parser, const char* at, size_t length);
    static int on_header_field(llhttp_t* parser, const char* at, size_t length);
    static int on_header_value(llhttp_t* parser, const char* at, size_t length);
    static int on_body(llhttp_t* parser, const char* at, size_t length);
    static int on_message_complete(llhttp_t* parser);

    static HttpContext* get_context(llhttp_t* parser) {
        return static_cast<HttpContext*>(parser->data);
    }

    void on_message_begin_impl();
    void on_url_impl(const char* at, size_t length);
    void on_header_field_impl(const char* at, size_t length);
    void on_header_value_impl(const char* at, size_t length);
    void on_body_impl(const char* at, size_t length);
    void on_message_complete_impl();

private:
    llhttp_t parser{};
    llhttp_settings_t settings{};

    HttpRequest request;

    bool messageComplete{ false };

    std::string currentHeaderField;
    std::string currentHeaderValue;
    bool lastWasValue{ false };
};

