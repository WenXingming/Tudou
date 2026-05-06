#include "HttpContext.h"


HttpContext::HttpContext() :
    parser_(),
    settings_(),
    request_(),
    messageComplete_(false),
    currentUrl_(),
    currentHeaderField_(),
    currentHeaderValue_(),
    lastWasValue_(false) {

    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &HttpContext::on_message_begin;
    settings_.on_url = &HttpContext::on_url;
    settings_.on_header_field = &HttpContext::on_header_field;
    settings_.on_header_value = &HttpContext::on_header_value;
    settings_.on_body = &HttpContext::on_body;
    settings_.on_message_complete = &HttpContext::on_message_complete;
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

bool HttpContext::parse(const char* data, size_t len, size_t& nparsed) {
    // 解析门面只负责执行状态机并返回契约结果，不在这里混入请求组装细节。
    const llhttp_errno_t err = llhttp_execute(&parser_, data, len);

    // llhttp 无错误时表示本批输入已经被当前状态机接收；是否完整由 messageComplete_ 单独表达。
    if (err == HPE_OK || err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) {
        nparsed = len;
        return true;
    }

    nparsed = 0;
    return false;
}

void HttpContext::reset() {
    reset_message_state();
    llhttp_reset(&parser_);
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
    // 每条新消息都必须在干净状态下构建，避免上一个请求的片段泄漏到当前请求。
    reset_message_state();
}

void HttpContext::on_url_impl(const char* at, size_t length) {
    // 请求行可能跨多次 parse 调用切分，先累计 target，再在 message complete 时一次性落盘。
    append_request_target_fragment(at, length);
    capture_request_method();
}

void HttpContext::on_header_field_impl(const char* at, size_t length) {
    // llhttp 允许头字段被分片回调，因此进入新 field 前必须先提交上一组已闭合的键值。
    flush_pending_header();
    currentHeaderField_.append(at, length);
}

void HttpContext::on_header_value_impl(const char* at, size_t length) {
    // Header Value 可能被多次回调拼接，必须保持 append 语义直到明确闭合。
    currentHeaderValue_.append(at, length);
    lastWasValue_ = true;
}

void HttpContext::on_body_impl(const char* at, size_t length) {
    // Body 可以被分块送达，因此请求体需要按序追加而不是覆盖。
    request_.append_body(at, length);
}

void HttpContext::on_message_complete_impl() {
    // 报文结束时统一提交尾 header 和请求行，确保请求对象只暴露完整状态。
    flush_pending_header();
    apply_request_target();
    capture_http_version();
    messageComplete_ = true;
}

void HttpContext::reset_message_state() {
    request_.clear();
    messageComplete_ = false;
    currentUrl_.clear();
    currentHeaderField_.clear();
    currentHeaderValue_.clear();
    lastWasValue_ = false;
}

void HttpContext::append_request_target_fragment(const char* at, size_t length) {
    currentUrl_.append(at, length);
}

void HttpContext::capture_request_method() {
    const char* method = llhttp_method_name(static_cast<llhttp_method>(parser_.method));
    if (method != nullptr) {
        request_.set_method(method);
    }
}

void HttpContext::apply_request_target() {
    request_.set_url(currentUrl_);

    const std::string::size_type querySeparator = currentUrl_.find('?');
    if (querySeparator == std::string::npos) {
        request_.set_path(currentUrl_);
        request_.set_query("");
        return;
    }

    request_.set_path(currentUrl_.substr(0, querySeparator));
    request_.set_query(currentUrl_.substr(querySeparator + 1));
}

void HttpContext::capture_http_version() {
    // HTTP 版本来自解析器最终状态，避免把所有请求都错误归一成 HTTP/1.1。
    request_.set_version("HTTP/" + std::to_string(parser_.http_major) + "." + std::to_string(parser_.http_minor));
}

void HttpContext::flush_pending_header() {
    if (!lastWasValue_) {
        return;
    }

    if (!currentHeaderField_.empty()) {
        request_.add_header(currentHeaderField_, currentHeaderValue_);
    }

    currentHeaderField_.clear();
    currentHeaderValue_.clear();
    lastWasValue_ = false;
}

