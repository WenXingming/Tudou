#pragma once

#include <string>

#include <random>
#include <sstream>

namespace filelink {

inline std::string generate_hex_uuid32() {
    // 32-char lowercase hex string
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dis(0, 15);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i) {
        oss << std::hex << dis(gen);
    }
    return oss.str();
}

}
