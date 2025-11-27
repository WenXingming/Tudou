#include "HttpRequest.h"

namespace tudou {

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

} // namespace tudou
