// ============================================================================
// HttpRequest.cpp
// HTTP 请求 DTO 实现，只处理字段读写与状态清空。
// ============================================================================

#include "tudou/http/HttpRequest.h"

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
    // DTO 语义下同名头字段以后写值覆盖前值，不在这里维护多值头聚合逻辑。
    headers_[field] = value;
}

const std::string& HttpRequest::get_header(const std::string& field) const {
    const auto it = headers_.find(field);
    if (it != headers_.end()) {
        return it->second;
    }

    // 缺失头字段统一返回稳定的空字符串引用，避免暴露悬空临时对象。
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

