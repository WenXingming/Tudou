// ============================================================================
// InetAddress.h
// IPv4 地址值对象，对 sockaddr_in 的构造、校验与读取契约做显式封装。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// InetAddress.h
// └── InetAddress
//     ├── InetAddress(ip, port)                  # [公有] 从 IPv4 文本和端口构造地址对象
//     ├── InetAddress(address)                   # [公有] 从原生 sockaddr_in 接管地址
//     ├── InetAddress(other)                     # [公有] 默认拷贝构造，保留地址值语义
//     ├── operator=(other)                       # [公有] 默认拷贝赋值
//     ├── ~InetAddress()                         # [公有] 默认析构
//     ├── get_sockaddr() const                   # [公有] 返回底层 sockaddr_in 视图
//     ├── get_ip() const                         # [公有] 输出 IPv4 文本地址
//     │   └── to_ip_string(address)              # [私有] 做二进制到文本转换
//     ├── get_port() const                       # [公有] 输出主机字节序端口
//     └── get_ip_port() const                    # [公有] 输出 ip:port 组合字符串
//         └── to_ip_string(address)              # [私有] 获取文本 IP
// ============================================================================

#pragma once

#include <netinet/in.h>
#include <string>

// InetAddress 负责将 IPv4 文本地址与原生 sockaddr_in 之间的转换收敛到单一契约对象中。
class InetAddress {
public:
    explicit InetAddress(const std::string& ip, uint16_t port);
    explicit InetAddress(const sockaddr_in& address);
    InetAddress(const InetAddress& other) = default;

    InetAddress& operator=(const InetAddress& other) = default;
    ~InetAddress() = default;

    const sockaddr_in& get_sockaddr() const;
    std::string get_ip() const;
    uint16_t get_port() const;
    std::string get_ip_port() const; // 输出统一格式的 ip:port 文本。

private:
    static std::string to_ip_string(const sockaddr_in& address);

private:
    sockaddr_in address_;               // 以网络字节序保存的 IPv4 地址契约，屏蔽调用方对底层结构体细节的直接操作。
};