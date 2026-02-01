/**
 * @file HttpResponse.cpp
 * @brief HTTP 响应报文封装类
 * @author wenxingming
 * @date 2025-11-27
 * @project: https://github.com/WenXingming/Tudou
 *
 */

#include "HttpResponse.h"

HttpResponse::HttpResponse() :
    httpVersion_("HTTP/1.1"),
    statusCode_(200),
    statusMessage_("OK"),
    headers_(),
    body_(),
    closeConnection_(false) {

}

std::string HttpResponse::package_to_string() const {
    std::string result;
    result.reserve(128 + body_.size());

    result.append(httpVersion_);
    result.push_back(' ');
    result.append(std::to_string(statusCode_));
    result.push_back(' ');
    result.append(statusMessage_);
    result.append("\r\n");

    for (const auto& kv : headers_) {
        result.append(kv.first);
        result.push_back(':');
        result.push_back(' ');
        result.append(kv.second);
        result.append("\r\n");
    }

    result.append("\r\n");
    result.append(body_);
    return std::move(result);
}

