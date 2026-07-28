// ============================================================================
// 验证 binary::Multiplexer 的序列号匹配、乱序完成与关闭唤醒。
// ============================================================================

#include <gtest/gtest.h>

#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

#include "tudou/rpc/binary/Multiplexer.h"

namespace tudou {
namespace rpc {
namespace test {

TEST(BinaryRpcMultiplexerTest, MatchesOutOfOrderResponses) {
    binary::Multiplexer multiplexer;
    std::vector<std::string> completed;

    const uint64_t first = multiplexer.register_call(
        [&completed](const std::string& body, std::exception_ptr) {
            completed.push_back(body);
        });
    const uint64_t second = multiplexer.register_call(
        [&completed](const std::string& body, std::exception_ptr) {
            completed.push_back(body);
        });

    EXPECT_TRUE(multiplexer.complete_call(second, "second"));
    EXPECT_TRUE(multiplexer.complete_call(first, "first"));
    EXPECT_EQ(completed, (std::vector<std::string>{"second", "first"}));
}

TEST(BinaryRpcMultiplexerTest, FailsPendingCallsWhenClosed) {
    binary::Multiplexer multiplexer;
    bool failed = false;
    multiplexer.register_call(
        [&failed](const std::string&, std::exception_ptr error) {
            failed = error != nullptr;
        });

    multiplexer.close(std::make_exception_ptr(std::runtime_error("closed")));

    EXPECT_TRUE(failed);
    EXPECT_FALSE(multiplexer.is_open());
    EXPECT_THROW(
        multiplexer.register_call(
            [](const std::string&, std::exception_ptr) {}),
        std::runtime_error);
}

} // namespace test
} // namespace rpc
} // namespace tudou
