/**
 * @file CoroutineTest.cpp
 * @brief 有栈协程上下文切换单元测试
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include <gtest/gtest.h>
#include "tudou/rpc/Coroutine.h"
#include <vector>

namespace tudou {
namespace rpc {
namespace test {

TEST(CoroutineTest, ContextSwitchingWorks) {
    std::vector<int> executionOrder;

    // 1. 实例化协程。构造函数执行后，协程内部代码立即运行至第 1 次 yield 挂起
    auto coro = std::make_shared<Coroutine>(nullptr, [&executionOrder]() {
        executionOrder.push_back(1); // 协程内部 1
        EXPECT_NE(Coroutine::t_current_coroutine, nullptr);
        
        Coroutine::t_current_coroutine->yield(); // 挂起协程，控制权返回给构造函数之后
        
        executionOrder.push_back(3); // 协程内部 2
    });

    // 2. 协程挂起后，回到主执行流
    executionOrder.push_back(2);
    EXPECT_EQ(Coroutine::t_current_coroutine, nullptr);

    // 3. 第一次显式 resume，唤醒协程继续运行直至结束
    coro->resume();
    executionOrder.push_back(4);

    // 预期执行流顺序应该是: 协程内(1) -> 构造返回后主线程(2) -> 唤醒后协程内(3) -> 协程结束后主线程(4)
    std::vector<int> expected = {1, 2, 3, 4};
    EXPECT_EQ(executionOrder, expected);
}

} // namespace test
} // namespace rpc
} // namespace tudou
