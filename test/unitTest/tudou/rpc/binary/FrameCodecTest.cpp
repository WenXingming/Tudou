// ============================================================================
// 验证二进制 RPC 帧的编解码、半包、粘包与非法输入处理。
// ============================================================================

#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <cstring>
#include <stdexcept>
#include <string>

#include "tudou/rpc/binary/FrameCodec.h"

namespace tudou {
namespace rpc {
namespace test {

TEST(FrameCodecTest, EncodesAndDecodesCompleteFrame) {
    const binary::Frame source(
        binary::FrameType::Request,
        42,
        "call-head",
        "request-body");

    Buffer buffer;
    buffer.write_to_buffer(binary::FrameCodec::encode(source));

    binary::Frame decoded;
    ASSERT_EQ(
        binary::FrameCodec::try_decode(buffer, decoded),
        binary::FrameCodec::DecodeResult::Complete);
    EXPECT_EQ(decoded.header.magic, binary::kMagic);
    EXPECT_EQ(decoded.header.version, binary::kVersion);
    EXPECT_EQ(decoded.header.type, binary::FrameType::Request);
    EXPECT_EQ(decoded.header.sequenceId, 42);
    EXPECT_EQ(decoded.header.headLength, 9);
    EXPECT_EQ(decoded.header.bodyLength, 12);
    EXPECT_EQ(decoded.head, "call-head");
    EXPECT_EQ(decoded.body, "request-body");
    EXPECT_EQ(buffer.readable_bytes(), 0);
}

TEST(FrameCodecTest, PreservesIncompleteFrameUntilMoreDataArrives) {
    const std::string bytes = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Response,
        7,
        "",
        "response"));

    Buffer buffer;
    buffer.write_to_buffer(bytes.data(), 10);

    binary::Frame decoded;
    EXPECT_EQ(
        binary::FrameCodec::try_decode(buffer, decoded),
        binary::FrameCodec::DecodeResult::NeedMoreData);
    EXPECT_EQ(buffer.readable_bytes(), 10);

    buffer.write_to_buffer(bytes.data() + 10, bytes.size() - 10);
    EXPECT_EQ(
        binary::FrameCodec::try_decode(buffer, decoded),
        binary::FrameCodec::DecodeResult::Complete);
    EXPECT_EQ(decoded.body, "response");
}

TEST(FrameCodecTest, DecodesStickyFramesOneByOne) {
    const std::string first = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Request, 1, "a", "first"));
    const std::string second = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Response, 2, "", "second"));

    Buffer buffer;
    buffer.write_to_buffer(first + second);

    binary::Frame decoded;
    ASSERT_EQ(
        binary::FrameCodec::try_decode(buffer, decoded),
        binary::FrameCodec::DecodeResult::Complete);
    EXPECT_EQ(decoded.header.sequenceId, 1);
    EXPECT_EQ(decoded.body, "first");

    ASSERT_EQ(
        binary::FrameCodec::try_decode(buffer, decoded),
        binary::FrameCodec::DecodeResult::Complete);
    EXPECT_EQ(decoded.header.sequenceId, 2);
    EXPECT_EQ(decoded.body, "second");
}

TEST(FrameCodecTest, RejectsInvalidMagicVersionAndType) {
    const std::string valid = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Request, 1, "", ""));

    for (size_t offset : {size_t(0), size_t(2), size_t(3)}) {
        std::string invalid = valid;
        invalid[offset] = static_cast<char>(0x7F);

        Buffer buffer;
        buffer.write_to_buffer(invalid);
        binary::Frame decoded;
        EXPECT_EQ(
            binary::FrameCodec::try_decode(buffer, decoded),
            binary::FrameCodec::DecodeResult::Invalid);
        EXPECT_EQ(buffer.readable_bytes(), invalid.size());
    }
}

TEST(FrameCodecTest, RejectsOversizedFrame) {
    std::string bytes = binary::FrameCodec::encode(binary::Frame(
        binary::FrameType::Request, 1, "", ""));
    const uint32_t oversizedLength = htonl(0xFFFFFFFFU);
    std::memcpy(&bytes[12], &oversizedLength, sizeof(oversizedLength));

    Buffer buffer;
    buffer.write_to_buffer(bytes);

    binary::Frame decoded;
    EXPECT_EQ(
        binary::FrameCodec::try_decode(buffer, decoded),
        binary::FrameCodec::DecodeResult::Invalid);
}

TEST(FrameCodecTest, RejectsHeaderThatDoesNotMatchPayload) {
    binary::Frame frame(binary::FrameType::Request, 1, "head", "body");
    ++frame.header.bodyLength;

    EXPECT_THROW(binary::FrameCodec::encode(frame), std::invalid_argument);
}

} // namespace test
} // namespace rpc
} // namespace tudou
