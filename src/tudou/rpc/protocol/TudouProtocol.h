/**
 * @file TudouProtocol.h
 * @brief Tudou 二进制 RPC 协议帧结构定义
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 *
 * Custom 20-Byte Binary Packet Frame Layout:
 *
 * 0                   1                   2                   3
 * 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |          Magic (2B)           |  Version (1B) |   Type (1B)   |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                                                               |
 * +                     Sequence Session ID (8B)                  +
 * |                                                               |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                        Meta Length (4B)                       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                        Body Length (4B)                       |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |          Meta Payload (Variable length, Meta Length)          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |          Body Payload (Variable length, Body Length)          |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 * Note: High-fidelity image layout is saved in:
 * docs/tudou_rpc_protocol.jpg
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace tudou {
namespace rpc {

// 协议头部魔数，用以快速识别合法请求 (TD = 0x5444)
constexpr uint16_t kRpcMagic = 0x5444;
constexpr uint8_t kRpcVersion = 1;

// 二进制包消息类型
enum class RpcMessageType : uint8_t {
    Request = 0,
    Response = 1,
    Heartbeat = 2
};

#pragma pack(push, 1)
struct RpcHeader {
    uint16_t magic;       // 0-1 字节: 协议魔数 (Big-Endian)
    uint8_t version;      // 2 字节: 协议版本号
    uint8_t type;         // 3 字节: 消息类型 (RpcMessageType)
    uint64_t sequenceId;  // 4-11 字节: 消息序列会话 ID (Big-Endian)
    uint32_t metaLen;     // 12-15 字节: Meta 信息长度 (Big-Endian)
    uint32_t bodyLen;     // 16-19 字节: Body 载荷长度 (Big-Endian)
};
#pragma pack(pop)

// 头部总大小固定为 20 字节
constexpr size_t kRpcHeaderSize = sizeof(RpcHeader);

} // namespace rpc
} // namespace tudou
