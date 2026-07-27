#include "tudou/http/HttpContext.h"

#include <cassert>

// ============================================================================
// 单连接 HTTP 请求解析上下文，持有 llhttp 状态机和正在构建的 HttpRequest。
// 将分片回调收敛为完整请求；不负责读取 TCP 数据、路由或发送响应。
// ============================================================================

HttpContext::HttpContext() :
    parser_(),
    settings_(),
    request_(),
    consumedBytes_(0),
    currentUrl_(),
    pendingHeaderField_(),
    pendingHeaderValue_(),
    hasPendingHeaderValue_(false) {

    llhttp_settings_init(&settings_);
    settings_.on_message_begin = &HttpContext::on_message_begin;
    settings_.on_url = &HttpContext::on_url;
    settings_.on_version_complete = &HttpContext::on_version_complete;
    settings_.on_header_field = &HttpContext::on_header_field;
    settings_.on_header_value = &HttpContext::on_header_value;
    settings_.on_headers_complete = &HttpContext::on_headers_complete;
    settings_.on_body = &HttpContext::on_body;
    settings_.on_message_complete = &HttpContext::on_message_complete;
    llhttp_init(&parser_, HTTP_REQUEST, &settings_);
    parser_.data = this;
}

HttpContext::ParseResult HttpContext::parse(const char* data, size_t len) {
    assert(data != nullptr || len == 0);
    consumedBytes_ = 0;

    const llhttp_errno_t err = llhttp_execute(&parser_, data, len);

    if (err == HPE_OK) {
        consumedBytes_ = len;
        return ParseResult::NeedMoreData;
    }
    else if (err == HPE_PAUSED || err == HPE_PAUSED_UPGRADE) {
        const char* errorPos = llhttp_get_error_pos(&parser_);
        if (errorPos != nullptr) {
            consumedBytes_ = static_cast<size_t>(errorPos - data);
        }
        else {
            consumedBytes_ = len;
        }
        return ParseResult::Complete;
    }

    return ParseResult::Rejected;
}

void HttpContext::reset() {
    reset_message_state();
    llhttp_reset(&parser_);
    llhttp_resume(&parser_);
    parser_.data = this;
}

int HttpContext::on_message_begin(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    ctx->reset_message_state();
    return 0;
}

int HttpContext::on_url(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->currentUrl_.append(at, length);
    return 0;
}

int HttpContext::on_version_complete(llhttp_t* parser) {
    auto* ctx = get_context(parser);

    // HTTP Method
    const char* method = llhttp_method_name(static_cast<llhttp_method>(parser->method));
    if (method != nullptr) {
        ctx->request_.set_method(method);
    }

    // HTTP URL, Path, Query
    ctx->request_.set_url(ctx->currentUrl_);
    const std::string::size_type querySeparator = ctx->currentUrl_.find('?');
    if (querySeparator == std::string::npos) {
        ctx->request_.set_path(ctx->currentUrl_);
        ctx->request_.set_query("");
    }
    else {
        ctx->request_.set_path(ctx->currentUrl_.substr(0, querySeparator));
        ctx->request_.set_query(ctx->currentUrl_.substr(querySeparator + 1));
    }

    // HTTP Version
    ctx->request_.set_version("HTTP/" + std::to_string(parser->http_major) + "." + std::to_string(parser->http_minor));

    return 0;
}

int HttpContext::on_header_field(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    if (ctx->hasPendingHeaderValue_) { // 不可直接在每次 on_header_value() 后提交，因为 Header value 本身也可能被分片回调
        ctx->commit_pending_header();
    }
    ctx->pendingHeaderField_.append(at, length);
    return 0;
}

int HttpContext::on_header_value(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->pendingHeaderValue_.append(at, length);
    ctx->hasPendingHeaderValue_ = true;
    return 0;
}

int HttpContext::on_headers_complete(llhttp_t* parser) {
    auto* ctx = get_context(parser);
    ctx->commit_pending_header();
    return 0;
}

int HttpContext::on_body(llhttp_t* parser, const char* at, size_t length) {
    auto* ctx = get_context(parser);
    ctx->request_.append_body(at, length);
    return 0;
}

int HttpContext::on_message_complete(llhttp_t*) {
    return HPE_PAUSED;
}

void HttpContext::commit_pending_header() {
    if (!hasPendingHeaderValue_) {
        return;
    }

    if (!pendingHeaderField_.empty()) {
        request_.add_header(pendingHeaderField_, pendingHeaderValue_);
    }

    pendingHeaderField_.clear();
    pendingHeaderValue_.clear();
    hasPendingHeaderValue_ = false;
}

void HttpContext::reset_message_state() {
    request_.clear();
    currentUrl_.clear();
    pendingHeaderField_.clear();
    pendingHeaderValue_.clear();
    hasPendingHeaderValue_ = false;
}
