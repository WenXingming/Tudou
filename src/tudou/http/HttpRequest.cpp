// ============================================== //
// HttpRequest.cpp
// HTTP 请求 DTO 实现，只处理字段读写与状态清空。
// ============================================== //

#include "HttpRequest.h"

namespace {

const std::string kEmptyHeaderValue;

} // namespace

HttpRequest::HttpRequest() :
    method_(),
    url_(),
    path_(),
    query_(),
    version_(),
    headers_(),
    body_() {
}

void HttpRequest::add_header(const std::string& field, const std::string& value) {
    headers_[field] = value;
}

const std::string& HttpRequest::get_header(const std::string& field) const {
    const auto it = headers_.find(field);
    if (it != headers_.end()) {
        return it->second;
    }

    return kEmptyHeaderValue;
}

void HttpRequest::clear() {
    // clear 会把 DTO 恢复成干净输入状态，避免上一条报文残留到下一次解析。
    method_.clear();
    url_.clear();
    path_.clear();
    query_.clear();
    version_.clear();
    headers_.clear();
    body_.clear();
}

