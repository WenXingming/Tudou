#pragma once

#include <string>

#include <cstddef>

namespace filelink {

namespace detail {

inline int hex_to_int(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string get_extension_lower(const std::string& filename) {
    const std::size_t pos = filename.rfind('.');
    if (pos == std::string::npos || pos + 1 >= filename.size()) {
        return std::string();
    }

    std::string ext = filename.substr(pos + 1);
    for (std::size_t i = 0; i < ext.size(); ++i) {
        if (ext[i] >= 'A' && ext[i] <= 'Z') {
            ext[i] = static_cast<char>(ext[i] - 'A' + 'a');
        }
    }
    return ext;
}

} // namespace detail

inline std::string guess_content_type_by_name(const std::string& filename) {
    const std::string ext = detail::get_extension_lower(filename);
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js") return "text/javascript; charset=utf-8";
    if (ext == "txt") return "text/plain; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "mp4") return "video/mp4";
    if (ext == "mp3") return "audio/mpeg";
    return "application/octet-stream";
}

// 仅用于解码 X-File-Name 等简单 header（%XX 形式）；遇到非法编码会原样保留。
inline std::string url_decode(const std::string& s) {
    std::string out;
    out.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            const int hi = detail::hex_to_int(s[i + 1]);
            const int lo = detail::hex_to_int(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (c == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(c);
    }

    return out;
}

// 非严格 JSON 转义：满足 demo 返回 url/id 的常见字符
inline std::string json_escape_minimal(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

}
