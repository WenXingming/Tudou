#include "HttpResponse.h"

namespace tudou {

std::string HttpResponse::package_to_string() const {
    std::string result;
    result.reserve(128 + body.size());

    result.append(httpVersion);
    result.push_back(' ');
    result.append(std::to_string(statusCode));
    result.push_back(' ');
    result.append(statusMessage);
    result.append("\r\n");

    for (const auto& kv : headers) {
        result.append(kv.first);
        result.push_back(':');
        result.push_back(' ');
        result.append(kv.second);
        result.append("\r\n");
    }

    result.append("\r\n");
    result.append(body);

    return result;
}

} // namespace tudou
