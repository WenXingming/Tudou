// ============================================================================
// FrameCodec 只负责完整 RPC 帧与网络字节之间的转换。
// 数据不足时不消费 Buffer；非法帧由调用方决定是否关闭连接。
// ============================================================================

#pragma once

#include <string>

#include "tudou/rpc/binary/Frame.h"
#include "tudou/tcp/Buffer.h"

namespace tudou {
namespace rpc {
namespace binary {

class FrameCodec {
public:
    enum class DecodeResult {
        Complete,
        NeedMoreData,
        Invalid
    };

    FrameCodec() = delete;

    static std::string encode(const Frame& frame);
    static DecodeResult try_decode(Buffer& buffer, Frame& frame);
};

} // namespace binary
} // namespace rpc
} // namespace tudou
