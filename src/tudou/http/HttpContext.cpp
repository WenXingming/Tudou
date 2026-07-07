#include "tudou/http/HttpContext.h"

#include <cassert>

// ============================================================================
// HttpContext.cpp
// HTTP 请求解析上下文实现，把 llhttp 回调流收敛成稳定的 HttpRequest。
// ============================================================================

HttpContext::HttpContext() :
    parser_(),
    settings_(),
    request_(),
    messageComplete_(false),
    currentUrl_(),
    currentHeaderField_(),
    currentHeaderValue_(),
    lastWasValue_(false) {

    // llhttp 的静态回调全部桥接回当前对象，保证解析器状态和请求构建状态始终同源。
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

HttpContext::ParseResult HttpContext::parse(const char* data, size_t len) {
    assert(data != nullptr || len == 0);

    // 解析门面只负责执行状态机并返回显式状态，不再把“是否完整”拆成额外查询契约。
    const llhttp_errno_t err = llhttp_execute(&parser_, data, len);

    // llhttp 无错误时表示本批输入已经被当前状态机接收；完整性直接体现在返回值里。
    if (err == HPE_OK || err == HPE_PAUSED_UPGRADE || err == HPE_PAUSED) {
        return messageComplete_ ? ParseResult::Complete : ParseResult::NeedMoreData;
    }

    return ParseResult::Rejected;
}

void HttpContext::reset() {
    // reset 同时清空 DTO 状态和 llhttp 内部状态机，确保下一条报文从干净边界开始。
    reset_message_state();
    llhttp_reset(&parser_);
    parser_.data = this;
}

int HttpContext::on_message_begin(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    ctx->reset_message_state(); // 每条新消息都必须在干净状态下构建，避免上一个请求的片段泄漏到当前请求。
    return 0;
}

int HttpContext::on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    // 请求行可能跨多次 parse 调用切分，先累计 target，再在 message complete 时一次性落盘。
    ctx->currentUrl_.append(at, length);

    const char* method = llhttp_method_name(static_cast<llhttp_method>(parser->method));
    if (method != nullptr) {
        ctx->request_.set_method(method);
    }
    return 0;
}

int HttpContext::on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    // llhttp 允许头字段被分片回调，因此进入新 field 前必须先提交上一组已闭合的键值。
    ctx->flush_pending_header();
    ctx->currentHeaderField_.append(at, length);
    return 0;
}

int HttpContext::on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    // Header Value 可能被多次回调拼接，必须保持 append 语义直到明确闭合。
    ctx->currentHeaderValue_.append(at, length);
    ctx->lastWasValue_ = true;
    return 0;
}

int HttpContext::on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    // Body 可以被分块送达，因此请求体需要按序追加而不是覆盖。
    ctx->request_.append_body(at, length);
    return 0;
}

int HttpContext::on_message_complete(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    // 报文结束时统一提交尾 header 和请求行，确保请求对象只暴露完整状态。
    ctx->flush_pending_header();

    ctx->request_.set_url(ctx->currentUrl_);
    const std::string::size_type querySeparator = ctx->currentUrl_.find('?');
    if (querySeparator == std::string::npos) {
        ctx->request_.set_path(ctx->currentUrl_);
        ctx->request_.set_query("");
    } else {
        ctx->request_.set_path(ctx->currentUrl_.substr(0, querySeparator));
        ctx->request_.set_query(ctx->currentUrl_.substr(querySeparator + 1));
    }

    ctx->request_.set_version("HTTP/" + std::to_string(parser->http_major) + "." + std::to_string(parser->http_minor));
    ctx->messageComplete_ = true;
    return 0;
}

void HttpContext::reset_message_state() {
    request_.clear();
    messageComplete_ = false;
    currentUrl_.clear();
    currentHeaderField_.clear();
    currentHeaderValue_.clear();
    lastWasValue_ = false;
}

void HttpContext::flush_pending_header() {
    if (!lastWasValue_) {
        return;
    }

    // 只有 field 和 value 都完成闭合后才真正落盘，避免分片 header 过早进入 DTO。
    if (!currentHeaderField_.empty()) {
        request_.add_header(currentHeaderField_, currentHeaderValue_);
    }

    currentHeaderField_.clear();
    currentHeaderValue_.clear();
    lastWasValue_ = false;
}

