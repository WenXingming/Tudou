/**
 * @file InetAddress.cpp
 * @brief 封装结构体 sockaddr_in
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "InetAddress.h"
#include <string.h>
#include <arpa/inet.h>
#include <sstream>

InetAddress::InetAddress(std::string _ip, uint16_t _port) {
    
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(_port);
    inet_pton(AF_INET, _ip.c_str(), &address.sin_addr);
    // address.sin_addr.s_addr = inet_addr(_ip.c_str()); // 基于字符串的地址初始化，返回网络序（大端）
    // address.sin_addr.s_addr = htonl(INADDR_ANY);
}

InetAddress::InetAddress(const sockaddr_in& _addr) {
    address = _addr;
}

const sockaddr_in& InetAddress::get_sockaddr() const {
    return address;
}

std::string InetAddress::get_ip() const {
    // 网络字节序的二进制 IP 地址转换为可读的字符串格式
    char buffer[64] = {};
    ::inet_ntop(AF_INET, &address.sin_addr, buffer, sizeof(buffer));
    return std::string(buffer);
}

uint16_t InetAddress::get_port() const {
    return ntohs(address.sin_port);
}

std::string InetAddress::get_ip_port() const {
    std::stringstream ss;
    ss << get_ip() << ":" << get_port();
    return ss.str();
}
