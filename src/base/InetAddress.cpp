/**
 * @file InetAddress.cpp
 * @brief 封装结构体 sockaddr_in
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#include "InetAddress.h"
#include <string.h>
#include <arpa/inet.h>
#include <sstream>

InetAddress::InetAddress(uint16_t _port, std::string _ip) {
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(_port);
    // address.sin_addr.s_addr = inet_addr(_ip.c_str()); // 基于字符串的地址初始化，返回网络序（大端）
    address.sin_addr.s_addr = htonl(INADDR_ANY);
}

sockaddr_in InetAddress::get_sockaddr() const {
    return address;
}

std::string InetAddress::get_ip() const {
    // 网络字节序的二进制IP地址转换为可读的字符串格式
    char buffer[64] = {};
    ::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return std::string(buffer);
}

std::string InetAddress::get_ip_port() const {
    std::string ipStr = get_ip();
    std::string portStr = std::to_string(address.sin_port);
    std::stringstream ss;
    ss << ipStr << ":" << portStr; // 忘了哪里看的字符串 + 效率比较低，所以用 sstream
    return ss.str();
}

uint16_t InetAddress::get_port() const {
    return address.sin_port;
}
