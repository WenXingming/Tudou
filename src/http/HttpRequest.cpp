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
    method(),
    url(),
    path(),
    query(),
    version(),
    headers(),
    body() {
}

void HttpRequest::add_header(const std::string& field, const std::string& value) {
    headers[field] = value;
}

const std::string& HttpRequest::get_header(const std::string& field) const {
    static const std::string empty;
    auto it = headers.find(field);
    if (it != headers.end()) {
        return it->second;
    }
    return empty;
}

void HttpRequest::clear() {
    method.clear();
    url.clear();
    path.clear();
    query.clear();
    version.clear();
    headers.clear();
    body.clear();
}

