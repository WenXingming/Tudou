// ============================================================================
// FrameCodec 读写固定 20 字节大端帧头，并保持半包解析零副作用。
// ============================================================================

#include "tudou/rpc/binary/FrameCodec.h"

#include <arpa/inet.h>
#include <endian.h>

#include <cstring>
#include <stdexcept>
#include <utility>

namespace tudou {
namespace rpc {
namespace binary {

namespace {

bool is_valid_type(FrameType type) {
    return type == FrameType::Request || type == FrameType::Response;
}

} // namespace

std::string FrameCodec::encode(const Frame& frame) {
    const size_t payloadSize = frame.head.size() + frame.body.size();
    if (payloadSize > kMaxFrameSize - kHeaderSize) {
        throw std::length_error("FrameCodec: Frame is too large");
    }
    if (frame.header.magic != kMagic
        || frame.header.version != kVersion
        || !is_valid_type(frame.header.type)
        || frame.header.headLength != frame.head.size()
        || frame.header.bodyLength != frame.body.size()) {
        throw std::invalid_argument("FrameCodec: Frame header does not match payload");
    }

    // 只转换副本，Frame 对象始终保持主机字节序，避免调用方看到混合状态。
    FrameHeader header = frame.header;
    header.magic = htons(header.magic);
    header.sequenceId = htobe64(header.sequenceId);
    header.headLength = htonl(header.headLength);
    header.bodyLength = htonl(header.bodyLength);

    std::string bytes(reinterpret_cast<const char*>(&header), kHeaderSize);
    bytes.append(frame.head);
    bytes.append(frame.body);
    return bytes;
}

FrameCodec::DecodeResult FrameCodec::try_decode(Buffer& buffer, Frame& frame) {
    if (buffer.readable_bytes() < kHeaderSize) {
        return DecodeResult::NeedMoreData;
    }

    // 先窥探帧头；只有确认整帧到齐后才推进 Buffer 读索引。
    FrameHeader header;
    std::memcpy(&header, buffer.readable_start_ptr(), kHeaderSize);

    if (ntohs(header.magic) != kMagic
        || header.version != kVersion
        || !is_valid_type(header.type)) {
        return DecodeResult::Invalid;
    }

    const size_t headLength = ntohl(header.headLength);
    const size_t bodyLength = ntohl(header.bodyLength);
    const size_t frameSize = kHeaderSize + headLength + bodyLength;
    if (frameSize > kMaxFrameSize) {
        return DecodeResult::Invalid;
    }

    if (buffer.readable_bytes() < frameSize) {
        return DecodeResult::NeedMoreData;
    }

    // 从这里开始消费 Buffer，半包路径在此前返回且不会丢失任何字节。
    buffer.advance_read_index(kHeaderSize);
    const uint64_t sequenceId = be64toh(header.sequenceId);
    std::string head = buffer.read_from_buffer(headLength);
    std::string body = buffer.read_from_buffer(bodyLength);
    frame = Frame(header.type, sequenceId, std::move(head), std::move(body));
    return DecodeResult::Complete;
}

} // namespace binary
} // namespace rpc
} // namespace tudou
