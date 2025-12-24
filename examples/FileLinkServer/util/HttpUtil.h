#pragma once

#include <string>

namespace filelink {

std::string guess_content_type_by_name(const std::string& filename);

// 仅用于解码 X-File-Name 等简单 header（%XX 形式）；遇到非法编码会原样保留。
std::string url_decode(const std::string& s);

// 非严格 JSON 转义：满足 demo 返回 url/id 的常见字符
std::string json_escape_minimal(const std::string& s);

}
