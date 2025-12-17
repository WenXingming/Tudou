/**
 * @file InetAddress.h
 * @brief 封装结构体 sockaddr_in
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once
#include <netinet/in.h>
#include <string>

class InetAddress {
private:
    sockaddr_in address;

public:
    explicit InetAddress(std::string _ip, uint16_t _port); // 不能隐式转换
    explicit InetAddress(const sockaddr_in& _addr);
    InetAddress(const InetAddress& other) = default;
    InetAddress& operator=(const InetAddress& other) = default;
    ~InetAddress() = default;

    const sockaddr_in& get_sockaddr() const;

    std::string get_ip() const;
    uint16_t get_port() const;
    std::string get_ip_port() const;
};