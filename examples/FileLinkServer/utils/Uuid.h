#pragma once

#include <string>

#include <random>
#include <sstream>

namespace filelink {

inline std::string generate_hex_uuid32() {
    // Generate a random UUID-like string
    // 该函数的功能是生成一个32字符的十六进制字符串（0-9，a-f），模拟 UUID 的格式
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dis(0, 15);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << dis(gen);
    }
    return oss.str();
}

}
