// ============================================================================ //
// InetAddress.h
// IPv4 地址值对象，对 sockaddr_in 的构造、校验与读取契约做显式封装。
// ============================================================================ //

#pragma once

#include <netinet/in.h>
#include <string>

// InetAddress 负责将 IPv4 文本地址与原生 sockaddr_in 之间的转换收敛到单一契约对象中。
class InetAddress {
public:
    explicit InetAddress(const std::string& ip, uint16_t port);
    explicit InetAddress(const sockaddr_in& address);
    InetAddress(const InetAddress& other) = default;

    /**
     * @brief 复制另一个 InetAddress 的地址契约。
     * @param other 被复制的地址对象。
     * @return InetAddress& 当前对象引用。
     */
    InetAddress& operator=(const InetAddress& other) = default;
    ~InetAddress() = default;

    /**
     * @brief 返回底层 sockaddr_in 视图。
     * @return const sockaddr_in& 以网络字节序保存的底层地址结构。
     */
    const sockaddr_in& get_sockaddr() const;

    /**
     * @brief 获取可读的 IPv4 文本地址。
     * @return std::string 点分十进制的 IPv4 文本。
     */
    std::string get_ip() const;

    /**
     * @brief 获取主机字节序表示的端口。
     * @return uint16_t 主机字节序端口值。
     */
    uint16_t get_port() const;

    /**
     * @brief 获取 ip:port 形式的组合字符串。
     * @return std::string 组合后的终端节点文本。
     */
    std::string get_ip_port() const;

private:
    /**
     * @brief 创建一个全零初始化的原生地址结构。
     * @return sockaddr_in 已清零的地址结构。
     */
    static sockaddr_in create_empty_address();

    /**
     * @brief 为原生地址结构写入 IPv4 地址族。
     * @param address 待写入的原生地址结构。
     */
    static void assign_family(sockaddr_in& address);

    /**
     * @brief 为原生地址结构写入网络字节序端口。
     * @param address 待写入的原生地址结构。
     * @param port 主机字节序端口值。
     */
    static void assign_port(sockaddr_in& address, uint16_t port);

    /**
     * @brief 为原生地址结构写入经过校验的 IPv4 文本地址。
     * @param address 待写入的原生地址结构。
     * @param ip 点分十进制 IPv4 文本。
     */
    static void assign_ip(sockaddr_in& address, const std::string& ip);

    /**
     * @brief 校验外部传入的原生地址是否属于 IPv4。
     * @param address 待校验的原生地址结构。
     */
    static void ensure_ipv4_family(const sockaddr_in& address);

    /**
     * @brief 将原生地址中的二进制 IP 转为可读文本。
     * @param address 已持有 IPv4 数据的原生地址结构。
     * @return std::string 点分十进制 IPv4 文本。
     */
    static std::string to_ip_string(const sockaddr_in& address);

    /**
     * @brief 将网络字节序端口还原为主机字节序。
     * @param address 已持有端口数据的原生地址结构。
     * @return uint16_t 主机字节序端口值。
     */
    static uint16_t read_port(const sockaddr_in& address);

    /**
     * @brief 格式化 ip:port 字符串输出。
     * @param ip 点分十进制 IPv4 文本。
     * @param port 主机字节序端口值。
     * @return std::string 标准化后的终端节点文本。
     */
    static std::string format_ip_port(const std::string& ip, uint16_t port);

private:
    sockaddr_in address_; // 以网络字节序保存的 IPv4 地址契约，屏蔽调用方对底层结构体细节的直接操作。
};