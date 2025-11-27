#include "HttpContext.h"

namespace tudou {

HttpContext::HttpContext() {
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
    auto err = llhttp_execute(&parser, data, len);
    // llhttp_execute 消费的是全部 len，如果中途错误会返回错误码。
    nparsed = (err == HPE_OK || err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) ? len : 0;
    return err == HPE_OK || err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED;
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
    std::string url(at, length);
    request.set_url(url);

    // 简单拆分 path 和 query
    auto pos = url.find('?');
    if (pos == std::string::npos) {
        request.set_path(url);
    }
    else {
        request.set_path(url.substr(0, pos));
        request.set_query(url.substr(pos + 1));
    }

    // method
    const char* method_str = llhttp_method_name(static_cast<llhttp_method>(parser.method));
    if (method_str) {
        request.set_method(method_str);
    }

    // version 统一用 HTTP/1.1
    request.set_version("HTTP/1.1");
}

void HttpContext::on_header_field_impl(const char* at, size_t length) {
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

} // namespace tudou
