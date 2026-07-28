// ============================================================================
// Frame 由 20 字节 FrameHeader、调用头和消息体组成。
// FrameHeader 在对象中使用主机字节序，FrameCodec 负责转换为网络大端序。
// ============================================================================

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace tudou {
namespace rpc {
namespace binary {

constexpr uint16_t kMagic = 0x5444;
constexpr uint8_t kVersion = 1;
constexpr size_t kMaxFrameSize = 64 * 1024 * 1024;

enum class FrameType : uint8_t {
    Request = 0,
    Response = 1
};

// ============================================================================
// FrameHeader 定义二进制 RPC 的 20 字节线上帧头。
// 保证 1 字节对齐，避免编译器在结构体中插入额外的填充字节
// ============================================================================
#pragma pack(push, 1)
struct FrameHeader {
    uint16_t magic = kMagic;
    uint8_t version = kVersion;
    FrameType type = FrameType::Request;
    uint64_t sequenceId = 0;
    uint32_t headLength = 0;
    uint32_t bodyLength = 0;
};
#pragma pack(pop)
constexpr size_t kHeaderSize = sizeof(FrameHeader);
static_assert(kHeaderSize == 20, "Binary RPC frame header must be exactly 20 bytes");


// ============================================================================
// Frame 按照线上顺序保存帧头、调用头和消息体。
// ============================================================================
struct Frame {
    Frame() : header(), head(), body() {
    }

    Frame(FrameType frameType, uint64_t frameSequenceId, std::string frameHead, std::string frameBody)
        : header(), head(std::move(frameHead)), body(std::move(frameBody)) {
        header.type = frameType;
        header.sequenceId = frameSequenceId;
        header.headLength = static_cast<uint32_t>(head.size());
        header.bodyLength = static_cast<uint32_t>(body.size());
    }

    FrameHeader header; // 20 字节帧头
    std::string head;   // 调用头，包括服务名、方法名
    std::string body;   // 消息体，包括请求参数或响应结果
};

} // namespace binary
} // namespace rpc
} // namespace tudou
