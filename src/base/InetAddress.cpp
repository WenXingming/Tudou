// ============================================================================ //
// InetAddress.cpp
// IPv4 地址值对象实现，显式展开构造步骤并收紧输入契约。
// ============================================================================ //

#include "InetAddress.h"

#include <arpa/inet.h>
#include <sstream>
#include <stdexcept>

InetAddress::InetAddress(const std::string& ip, uint16_t port) {
    // 构造流程按固定顺序线性展开，保证 family、port、ip 的写入语义一眼可见。
    address_ = create_empty_address();
    assign_family(address_);
    assign_port(address_, port);
    assign_ip(address_, ip);
}

InetAddress::InetAddress(const sockaddr_in& address) {
    // 外部若传入了原生地址结构，必须先确认它仍然满足 IPv4 契约，再接管副本。
    ensure_ipv4_family(address);
    address_ = address;
}

const sockaddr_in& InetAddress::get_sockaddr() const {
    return address_;
}

std::string InetAddress::get_ip() const {
    // 对外暴露文本 IP 时，统一复用单一序列化逻辑，避免字节序细节泄漏到调用方。
    return to_ip_string(address_);
}

uint16_t InetAddress::get_port() const {
    // 对外读取端口时统一转换为主机字节序，保证上层拿到的是业务可读值。
    return read_port(address_);
}

std::string InetAddress::get_ip_port() const {
    // 组合日志与调试输出时复用统一格式，避免不同调用点各自拼接出不一致的文本。
    return format_ip_port(to_ip_string(address_), read_port(address_));
}

sockaddr_in InetAddress::create_empty_address() {
    // 先得到一个零值结构，避免任何字段带着未定义内容进入后续系统调用。
    return sockaddr_in{};
}

void InetAddress::assign_family(sockaddr_in& address) {
    // InetAddress 的职责只覆盖 IPv4，因此地址族必须在第一时间固定为 AF_INET。
    address.sin_family = AF_INET;
}

void InetAddress::assign_port(sockaddr_in& address, uint16_t port) {
    // 成员内部统一保存网络字节序，避免同一个对象在不同位置混用两种端口表示。
    address.sin_port = htons(port);
}

void InetAddress::assign_ip(sockaddr_in& address, const std::string& ip) {
    // 文本 IP 必须在进入对象边界时完成解析与校验，拒绝把无效地址静默带入运行期。
    const int convertResult = inet_pton(AF_INET, ip.c_str(), &address.sin_addr);
    if (convertResult != 1) {
        throw std::invalid_argument("InetAddress requires a valid IPv4 text address: " + ip);
    }
}

void InetAddress::ensure_ipv4_family(const sockaddr_in& address) {
    // 从外部接管原生地址结构时，只接受已经明确标注为 IPv4 的契约输入。
    if (address.sin_family != AF_INET) {
        throw std::invalid_argument("InetAddress requires an AF_INET sockaddr_in input");
    }
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

uint16_t InetAddress::read_port(const sockaddr_in& address) {
    // 成员内部保存的是网络字节序端口，这里是唯一的对外解码出口。
    return ntohs(address.sin_port);
}

std::string InetAddress::format_ip_port(const std::string& ip, uint16_t port) {
    // 把 endpoint 展示规则固化在值对象内部，避免日志、调试和业务输出出现多份拼接实现。
    std::ostringstream endpoint;
    endpoint << ip << ":" << port;
    return endpoint.str();
}
