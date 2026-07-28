// ============================================================================
// Connection 累积本连接字节，并一次返回当前所有完整帧。
// ============================================================================

#include "tudou/rpc/binary/Connection.h"

#include <utility>

#include "tudou/rpc/binary/FrameCodec.h"

namespace tudou {
namespace rpc {
namespace binary {

Connection::Connection()
    : inputBuffer_() {
}

Connection::~Connection() = default;

bool Connection::decode(const std::string& data, std::vector<Frame>& frames) {
    // inputBuffer_ 保留上次未组成完整帧的字节，新数据只需追加到末尾。
    inputBuffer_.write_to_buffer(data);
    frames.clear();

    // 一次读事件可能包含多个粘连帧，持续解析到半包或非法数据为止。
    while (true) {
        Frame frame;
        const auto result = FrameCodec::try_decode(inputBuffer_, frame);
        switch (result) {
        case FrameCodec::DecodeResult::Complete:
            frames.push_back(std::move(frame));
            continue;
        case FrameCodec::DecodeResult::NeedMoreData:
            return true;
        case FrameCodec::DecodeResult::Invalid:
            return false;
        }
    }
}

} // namespace binary
} // namespace rpc
} // namespace tudou
