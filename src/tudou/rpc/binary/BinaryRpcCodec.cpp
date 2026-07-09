/**
 * @file BinaryRpcCodec.cpp
 * @brief Tudou 二进制 RPC 帧编解码器实现
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "BinaryRpcCodec.h"
#include <endian.h>
#include <arpa/inet.h>
#include <cstring>
#include <algorithm>

namespace tudou {
namespace rpc {
namespace binary {

void BinaryRpcCodec::encode(Buffer* buf,
                            RpcMessageType type,
                            uint64_t sequenceId,
                            const std::string& metaBytes,
                            const std::string& bodyBytes) {
    RpcHeader header;
    header.magic = htons(kRpcMagic);
    header.version = kRpcVersion;
    header.type = static_cast<uint8_t>(type);
    header.sequenceId = htobe64(sequenceId);
    header.metaLen = htonl(static_cast<uint32_t>(metaBytes.size()));
    header.bodyLen = htonl(static_cast<uint32_t>(bodyBytes.size()));

    // 1. 写入固定 20 字节头部
    buf->write_to_buffer(reinterpret_cast<const char*>(&header), kRpcHeaderSize);

    // 2. 写入元信息 (Meta)
    if (!metaBytes.empty()) {
        buf->write_to_buffer(metaBytes.data(), metaBytes.size());
    }

    // 3. 写入消息体 (Body)
    if (!bodyBytes.empty()) {
        buf->write_to_buffer(bodyBytes.data(), bodyBytes.size());
    }
}

BinaryRpcCodec::DecodeResult BinaryRpcCodec::decode(Buffer* buf,
                                                    RpcHeader& outHeader,
                                                    std::string& outMetaBytes,
                                                    std::string& outBodyBytes) {
    // 缓冲区大小不足以解析出一个固定头部 (20 字节)
    if (buf->readable_bytes() < kRpcHeaderSize) {
        return DecodeResult::Empty;
    }

    // 1. 预先窥探（peek）数据头部，不推进读指针，防止因为后续半包导致读指针割裂
    RpcHeader header;
    std::memcpy(&header, buf->readable_start_ptr(), kRpcHeaderSize);

    // 2. 解析校验网络魔数
    uint16_t magic = ntohs(header.magic);
    if (magic != kRpcMagic) {
        return DecodeResult::Error; // 协议魔数损坏或收到非法包
    }

    uint32_t metaLen = ntohl(header.metaLen);
    uint32_t bodyLen = ntohl(header.bodyLen);
    size_t totalPacketSize = kRpcHeaderSize + metaLen + bodyLen;

    // 3. 校验当前缓冲区可读字节数是否足以拼成一个完整包
    if (buf->readable_bytes() < totalPacketSize) {
        return DecodeResult::HalfPack; // 半包，等待下次数据到达
    }

    // 4. 正式消费头部，推进读指针
    buf->advance_read_index(kRpcHeaderSize);

    // 5. 组装元数据与载荷字节流并消费对应空间
    if (metaLen > 0) {
        outMetaBytes.assign(buf->readable_start_ptr(), metaLen);
        buf->advance_read_index(metaLen);
    } else {
        outMetaBytes.clear();
    }

    if (bodyLen > 0) {
        outBodyBytes.assign(buf->readable_start_ptr(), bodyLen);
        buf->advance_read_index(bodyLen);
    } else {
        outBodyBytes.clear();
    }

    // 6. 将帧头网络字节序还原回主机字节序导出
    outHeader.magic = magic;
    outHeader.version = header.version;
    outHeader.type = header.type;
    outHeader.sequenceId = be64toh(header.sequenceId);
    outHeader.metaLen = metaLen;
    outHeader.bodyLen = bodyLen;

    return DecodeResult::Success;
}

} // namespace binary
} // namespace rpc
} // namespace tudou
