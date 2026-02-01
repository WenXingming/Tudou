/**
 * @file HttpRequest.cpp
 * @brief HTTP 请求报文封装类
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "HttpRequest.h"

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
    static const std::string empty;
    auto it = headers_.find(field);
    if (it != headers_.end()) {
        return it->second;
    }
    return empty;
}

void HttpRequest::clear() {
    method_.clear();
    url_.clear();
    path_.clear();
    query_.clear();
    version_.clear();
    headers_.clear();
    body_.clear();
}

