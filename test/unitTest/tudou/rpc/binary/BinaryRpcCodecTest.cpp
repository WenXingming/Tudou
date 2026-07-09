/**
 * @file BinaryRpcCodecTest.cpp
 * @brief Tudou 二进制 RPC 帧编解码器单元测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/binary/BinaryRpcCodec.h"

using namespace tudou;
using namespace tudou::rpc::binary;

class BinaryRpcCodecTest : public ::testing::Test {
protected:
    Buffer buffer;
};

// 1. 验证常规完整 RPC 帧的编码与解码成功
TEST_F(BinaryRpcCodecTest, EncodesAndDecodesValidFrameSuccessfully) {
    uint64_t seq = 0x123456789ABCDEF0ULL;
    std::string meta = "UserService.Login";
    std::string body = "{\n  \"username\": \"alice\"\n}";

    // 编码写入缓冲区
    BinaryRpcCodec::encode(&buffer, RpcMessageType::Request, seq, meta, body);

    EXPECT_GT(buffer.readable_bytes(), kRpcHeaderSize);

    // 解码并还原
    RpcHeader outHeader;
    std::string outMeta;
    std::string outBody;
    BinaryRpcCodec::DecodeResult result = BinaryRpcCodec::decode(&buffer, outHeader, outMeta, outBody);

    ASSERT_EQ(result, BinaryRpcCodec::DecodeResult::Success);
    EXPECT_EQ(outHeader.magic, kRpcMagic);
    EXPECT_EQ(outHeader.version, kRpcVersion);
    EXPECT_EQ(static_cast<RpcMessageType>(outHeader.type), RpcMessageType::Request);
    EXPECT_EQ(outHeader.sequenceId, seq);
    EXPECT_EQ(outHeader.metaLen, meta.size());
    EXPECT_EQ(outHeader.bodyLen, body.size());
    EXPECT_EQ(outMeta, meta);
    EXPECT_EQ(outBody, body);
    EXPECT_EQ(buffer.readable_bytes(), 0); // 缓冲区数据应该被完全消费
}

// 2. 验证流式数据半包时，解包能够正确挂起且不损坏/丢弃数据
TEST_F(BinaryRpcCodecTest, HandlesHalfPackSuspensionCorrectly) {
    uint64_t seq = 8888;
    std::string meta = "HelloService.Say";
    std::string body = "payload_data";

    // 制作一个完整的二进制包
    Buffer tempBuffer;
    BinaryRpcCodec::encode(&tempBuffer, RpcMessageType::Response, seq, meta, body);
    std::string rawData = tempBuffer.read_from_buffer(); // 提取出全部字节流

    RpcHeader outHeader;
    std::string outMeta;
    std::string outBody;

    // A. 阶段一：只写入 10 个字节（连 20 字节头部都没收齐）
    buffer.write_to_buffer(rawData.data(), 10);
    EXPECT_EQ(BinaryRpcCodec::decode(&buffer, outHeader, outMeta, outBody), BinaryRpcCodec::DecodeResult::Empty);
    EXPECT_EQ(buffer.readable_bytes(), 10); // 读指针绝不能发生推进

    // B. 阶段二：写入剩余头部，以及 Meta 的前 5 字节（收齐头部但 Body/Meta 还没收全）
    // 此时 buffer 中包含 10 + 12 = 22 字节（20字节头部 + 2字节meta）
    buffer.write_to_buffer(rawData.data() + 10, 12);
    EXPECT_EQ(BinaryRpcCodec::decode(&buffer, outHeader, outMeta, outBody), BinaryRpcCodec::DecodeResult::HalfPack);
    EXPECT_EQ(buffer.readable_bytes(), 22); // 读指针仍不能发生推进

    // C. 阶段三：将剩余字节流倾泻写入（收全数据）
    buffer.write_to_buffer(rawData.data() + 22, rawData.size() - 22);
    EXPECT_EQ(BinaryRpcCodec::decode(&buffer, outHeader, outMeta, outBody), BinaryRpcCodec::DecodeResult::Success);

    // 断言解析值全部正确还原
    EXPECT_EQ(outHeader.sequenceId, seq);
    EXPECT_EQ(outMeta, meta);
    EXPECT_EQ(outBody, body);
    EXPECT_EQ(buffer.readable_bytes(), 0);
}

// 3. 验证网络粘包情况下，循环解包能够精准逐个提取
TEST_F(BinaryRpcCodecTest, DemuxesStickyPackagesCorrectly) {
    // 编码包 1
    BinaryRpcCodec::encode(&buffer, RpcMessageType::Request, 100, "Method1", "Body1");
    // 直接粘着编码包 2
    BinaryRpcCodec::encode(&buffer, RpcMessageType::Response, 200, "Method2", "Body2_Longer");

    RpcHeader header1, header2;
    std::string meta1, meta2;
    std::string body1, body2;

    // 解析包 1
    ASSERT_EQ(BinaryRpcCodec::decode(&buffer, header1, meta1, body1), BinaryRpcCodec::DecodeResult::Success);
    EXPECT_EQ(header1.sequenceId, 100);
    EXPECT_EQ(meta1, "Method1");
    EXPECT_EQ(body1, "Body1");

    // 解析包 2
    ASSERT_EQ(BinaryRpcCodec::decode(&buffer, header2, meta2, body2), BinaryRpcCodec::DecodeResult::Success);
    EXPECT_EQ(header2.sequenceId, 200);
    EXPECT_EQ(meta2, "Method2");
    EXPECT_EQ(body2, "Body2_Longer");

    EXPECT_EQ(buffer.readable_bytes(), 0);
}

// 4. 验证协议损坏或非法请求（Magic 不匹配）时能够熔断返回 Error
TEST_F(BinaryRpcCodecTest, ErrorsOutOnMagicMismatch) {
    // 写入 25 个无效字节（首部 magic 损坏）
    std::string garbageBytes(25, 'x');
    buffer.write_to_buffer(garbageBytes);

    RpcHeader outHeader;
    std::string outMeta;
    std::string outBody;

    EXPECT_EQ(BinaryRpcCodec::decode(&buffer, outHeader, outMeta, outBody), BinaryRpcCodec::DecodeResult::Error);
    EXPECT_EQ(buffer.readable_bytes(), 25); // 数据未被退回/消费（交给上层处理或切断连接）
}
