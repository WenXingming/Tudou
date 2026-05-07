// ============================================================================
// InetAddress.h
// IPv4 地址值对象，对 sockaddr_in 的构造、校验与读取契约做显式封装。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// InetAddress.h
// └── InetAddress
//     ├── InetAddress(ip, port)                  # [公有] 从 IPv4 文本和端口构造地址对象
//     │   ├── create_empty_address()             # [私有] 生成零值 sockaddr_in
//     │   ├── assign_family(address)             # [私有] 固定 AF_INET
//     │   ├── assign_port(address, port)         # [私有] 写入网络字节序端口
//     │   └── assign_ip(address, ip)             # [私有] 解析并校验 IPv4 文本地址
//     ├── InetAddress(address)                   # [公有] 从原生 sockaddr_in 接管地址
//     │   └── ensure_ipv4_family(address)        # [私有] 校验输入确为 IPv4
//     ├── InetAddress(other)                     # [公有] 默认拷贝构造，保留地址值语义
//     ├── operator=(other)                       # [公有] 默认拷贝赋值
//     ├── ~InetAddress()                         # [公有] 默认析构
//     ├── get_sockaddr() const                   # [公有] 返回底层 sockaddr_in 视图
//     ├── get_ip() const                         # [公有] 输出 IPv4 文本地址
//     │   └── to_ip_string(address)              # [私有] 做二进制到文本转换
//     ├── get_port() const                       # [公有] 输出主机字节序端口
//     │   └── read_port(address)                 # [私有] 还原端口字节序
//     ├── get_ip_port() const                    # [公有] 输出 ip:port 组合字符串
//     │   ├── to_ip_string(address)              # [私有] 获取文本 IP
//     │   ├── read_port(address)                 # [私有] 获取主机字节序端口
//     │   └── format_ip_port(ip, port)           # [私有] 统一格式化 endpoint
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
    static sockaddr_in create_empty_address();
    static void assign_family(sockaddr_in& address);
    static void assign_port(sockaddr_in& address, uint16_t port);
    static void assign_ip(sockaddr_in& address, const std::string& ip);
    static void ensure_ipv4_family(const sockaddr_in& address);
    static std::string to_ip_string(const sockaddr_in& address);
    static uint16_t read_port(const sockaddr_in& address);
    static std::string format_ip_port(const std::string& ip, uint16_t port);

private:
    sockaddr_in address_; // 以网络字节序保存的 IPv4 地址契约，屏蔽调用方对底层结构体细节的直接操作。
};