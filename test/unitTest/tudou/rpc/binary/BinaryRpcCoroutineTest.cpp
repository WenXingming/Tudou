// ============================================================================
// 验证 binary::Coroutine 的挂起、恢复与当前协程查询。
// ============================================================================

#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <vector>

#include "tudou/rpc/binary/Coroutine.h"

namespace tudou {
namespace rpc {
namespace test {

TEST(BinaryRpcCoroutineTest, SwitchesExecutionContext) {
    std::vector<int> order;
    auto coroutine = std::make_shared<binary::Coroutine>(nullptr, [&order]() {
        order.push_back(1);
        ASSERT_NE(binary::Coroutine::current(), nullptr);
        binary::Coroutine::current()->yield();
        order.push_back(3);
    });

    coroutine->resume();
    order.push_back(2);
    EXPECT_EQ(binary::Coroutine::current(), nullptr);

    coroutine->resume();
    order.push_back(4);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
}

TEST(BinaryRpcCoroutineTest, RestoresCurrentCoroutineAfterException) {
    auto coroutine = std::make_shared<binary::Coroutine>(nullptr, []() {
        throw std::runtime_error("failure");
    });

    EXPECT_THROW(coroutine->resume(), std::runtime_error);
    EXPECT_EQ(binary::Coroutine::current(), nullptr);
}

} // namespace test
} // namespace rpc
} // namespace tudou
