/**
 * @file InetAddress.h
 * @brief 封装结构体 sockaddr_in
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#pragma once
#include <netinet/in.h>
#include <string>

class InetAddress {
private:
    sockaddr_in address;

public:
    explicit InetAddress(uint16_t _port, std::string _ip = "127.0.0.1");
    explicit InetAddress(const sockaddr_in& _addr) // 可以拷贝
        : address(_addr) {}
    ~InetAddress() {}
    // explicit InetAddress(const InetAddress& other) = default;

    sockaddr_in get_sockaddr() const;

    std::string get_ip() const;
    std::string get_ip_port() const;
    uint16_t get_port() const;
};