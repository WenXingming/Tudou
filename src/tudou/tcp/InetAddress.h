// ============================================================================
// InetAddress 封装 IPv4 地址值，负责 sockaddr_in 的构造、校验和格式转换。
// 它不拥有 socket fd，只为网络层提供可复制的地址数据。
// ============================================================================

#pragma once

#include <netinet/in.h>
#include <string>

class InetAddress {
public:
    explicit InetAddress(const std::string& ip, uint16_t port);
    explicit InetAddress(const sockaddr_in& address);

    const sockaddr_in& get_sockaddr() const; // 返回底层地址视图，供 socket 系统调用使用。
    std::string get_ip() const;
    uint16_t get_port() const;
    std::string get_ip_port() const; // 输出统一格式的 ip:port 文本。

private:
    static std::string to_ip_string(const sockaddr_in& address);

private:
    sockaddr_in address_;               // 以网络字节序保存的 IPv4 地址契约，屏蔽调用方对底层结构体细节的直接操作。
};
