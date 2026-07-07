// ============================================================================
// InetAddress.cpp
// IPv4 地址值对象实现，显式展开构造步骤并收紧输入契约。
// ============================================================================

#include "InetAddress.h"

#include <arpa/inet.h>
#include <sstream>
#include <stdexcept>

InetAddress::InetAddress(const std::string& ip, uint16_t port) {
    address_ = sockaddr_in{};
    address_.sin_family = AF_INET;
    address_.sin_port = htons(port);
    if (::inet_pton(AF_INET, ip.c_str(), &address_.sin_addr) != 1) {
        throw std::invalid_argument("InetAddress requires a valid IPv4 text address: " + ip);
    }
}

InetAddress::InetAddress(const sockaddr_in& address) {
    if (address.sin_family != AF_INET) { // 检查满足 IPv4 契约
        throw std::invalid_argument("InetAddress requires an AF_INET sockaddr_in input");
    }
    address_ = address;
}

const sockaddr_in& InetAddress::get_sockaddr() const {
    return address_;
}

std::string InetAddress::get_ip() const {
    return to_ip_string(address_); // 对外暴露文本 IP 时，统一复用单一序列化逻辑，避免字节序细节泄漏到调用方。
}

uint16_t InetAddress::get_port() const {
    return ntohs(address_.sin_port); // 对外读取端口时统一转换为主机字节序，保证上层拿到的是业务可读值。
}

std::string InetAddress::get_ip_port() const {
    std::ostringstream endpoint;
    endpoint << to_ip_string(address_) << ":" << ntohs(address_.sin_port);
    return endpoint.str();
}

std::string InetAddress::to_ip_string(const sockaddr_in& address) {
    // 统一通过 inet_ntop 做二进制到文本的转换，避免外部自行处理缓冲区和协议细节。
    char buffer[INET_ADDRSTRLEN] = {};
    const char* convertedIp = ::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    if (convertedIp == nullptr) {
        throw std::invalid_argument("InetAddress failed to convert IPv4 address to text");
    }
    return std::string(convertedIp);
}
