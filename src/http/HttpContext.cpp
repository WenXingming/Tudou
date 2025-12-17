/**
 * @file HttpContext.cpp
 * @brief HTTP 报文解析上下文封装类
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include <iostream>
#include "HttpContext.h"


HttpContext::HttpContext() :
    parser(),
    settings(),
    request(),
    messageComplete(false),
    currentHeaderField(),
    currentHeaderValue(),
    lastWasValue(false) {
    
    llhttp_settings_init(&settings);
    settings.on_message_begin = &HttpContext::on_message_begin;
    settings.on_url = &HttpContext::on_url;
    settings.on_header_field = &HttpContext::on_header_field;
    settings.on_header_value = &HttpContext::on_header_value;
    settings.on_body = &HttpContext::on_body;
    settings.on_message_complete = &HttpContext::on_message_complete;

    llhttp_init(&parser, HTTP_REQUEST, &settings);
    parser.data = this;
}

bool HttpContext::parse(const char* data, size_t len, size_t& nparsed) {
    // llhttp_execute 消费的是全部 len，如果中途错误会返回错误码
    auto err = llhttp_execute(&parser, data, len);
    // 只要无错误即视为消费了全部输入数据即 len 字节（未完成的消息会保留在 llhttp 内部状态中，等待下一次 execute 调用继续解析）
    if (err == HPE_OK || err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) {
        nparsed = len;
        return true;
    }

    nparsed = 0;
    return false;
}

void HttpContext::reset() {
    request.clear();
    messageComplete = false;
    currentHeaderField.clear();
    currentHeaderValue.clear();
    lastWasValue = false;
    llhttp_reset(&parser);
}

int HttpContext::on_message_begin(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    ctx->on_message_begin_impl();
    return 0;
}

int HttpContext::on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->on_url_impl(at, length);
    return 0;
}

int HttpContext::on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->on_header_field_impl(at, length);
    return 0;
}

int HttpContext::on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->on_header_value_impl(at, length);
    return 0;
}

int HttpContext::on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->on_body_impl(at, length);
    return 0;
}

int HttpContext::on_message_complete(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    ctx->on_message_complete_impl();
    return 0;
}

void HttpContext::on_message_begin_impl() {
    request.clear();
    messageComplete = false;
    currentHeaderField.clear();
    currentHeaderValue.clear();
    lastWasValue = false;
}

void HttpContext::on_url_impl(const char* at, size_t length) {
    // 解析 method、url、path、query、version
    // Method
    const char* method_str = llhttp_method_name(static_cast<llhttp_method>(parser.method));
    if (method_str) {
        request.set_method(method_str);
    }

    // URL
    std::string url(at, length);
    request.set_url(url);

    // 简单拆分 URL 的 path 和 query
    auto pos = url.find('?');
    if (pos == std::string::npos) {
        request.set_path(url);
    }
    else {
        request.set_path(url.substr(0, pos));
        request.set_query(url.substr(pos + 1));
    }

    // version 统一用 HTTP/1.1
    request.set_version("HTTP/1.1");
}

void HttpContext::on_header_field_impl(const char* at, size_t length) {
    // 上一个是 value，说明一个完整的 header field-value 对已经结束
    if (lastWasValue) {
        if (!currentHeaderField.empty()) {
            request.add_header(currentHeaderField, currentHeaderValue);
        }
        currentHeaderField.clear();
        currentHeaderValue.clear();
        lastWasValue = false;
    }
    currentHeaderField.append(at, length);
}

void HttpContext::on_header_value_impl(const char* at, size_t length) {
    // 并非每次调用都是新的 value，可能是分块传输。不知道 value 是否会被分块传输，所以每次都 append。
    // 因为不知道 value 的结尾，所以只能等到下一个 header field 或消息结束时再保存。
    currentHeaderValue.append(at, length);
    lastWasValue = true;
}

void HttpContext::on_body_impl(const char* at, size_t length) {
    request.append_body(at, length);
}

void HttpContext::on_message_complete_impl() {
    if (!currentHeaderField.empty()) {
        request.add_header(currentHeaderField, currentHeaderValue);
    }
    messageComplete = true;
}

