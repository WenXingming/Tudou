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
    inputBuffer_.write_to_buffer(data); // 将新数据追加到缓冲区（每条连接必须拥有独立的半包缓存，避免数据错乱和丢失。llhttp 因为会自动保存半包数据，所以不需要额外的半包缓存）
    frames.clear();
    while (true) {
        Frame frame;
        const auto result = FrameCodec::try_decode(inputBuffer_, frame);
        if (result == FrameCodec::DecodeResult::Complete) {
            frames.push_back(std::move(frame));
            continue;
        }
        if (result == FrameCodec::DecodeResult::NeedMoreData) {
            return true;
        }
        return false;
    }
}

} // namespace binary
} // namespace rpc
} // namespace tudou
