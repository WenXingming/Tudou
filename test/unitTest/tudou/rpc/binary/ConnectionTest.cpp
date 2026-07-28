// ============================================================================
// 验证 binary::Connection 跨多次输入保留半包，并一次返回全部粘连帧。
// ============================================================================

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "tudou/rpc/binary/FrameCodec.h"
#include "tudou/rpc/binary/Connection.h"

namespace tudou {
namespace rpc {
namespace test {

TEST(BinaryRpcConnectionTest, DecodesFragmentedAndStickyFrames) {
    const std::string first = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Request, 1, "head", "first"));
    const std::string second = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Request, 2, "head", "second"));

    binary::Connection connection;
    std::vector<binary::Frame> frames;
    const size_t split = first.size() / 2;

    EXPECT_TRUE(connection.decode(first.substr(0, split), frames));
    EXPECT_TRUE(frames.empty());

    EXPECT_TRUE(connection.decode(first.substr(split) + second, frames));
    ASSERT_EQ(frames.size(), 2);
    EXPECT_EQ(frames[0].header.sequenceId, 1);
    EXPECT_EQ(frames[1].header.sequenceId, 2);
}

TEST(BinaryRpcConnectionTest, RejectsInvalidStream) {
    binary::Connection connection;
    std::vector<binary::Frame> frames;
    EXPECT_FALSE(connection.decode(
        std::string(binary::kHeaderSize, 'x'), frames));
}

} // namespace test
} // namespace rpc
} // namespace tudou
