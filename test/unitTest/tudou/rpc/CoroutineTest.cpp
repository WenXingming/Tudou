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

    // 1. 实例化协程。构造函数完成后，当前协程处于初始挂起状态
    auto coro = std::make_shared<Coroutine>(nullptr, [&executionOrder]() {
        executionOrder.push_back(1); // 协程内部 1
        EXPECT_NE(Coroutine::t_current_coroutine, nullptr);
        
        Coroutine::t_current_coroutine->yield(); // 挂起协程
        
        executionOrder.push_back(3); // 协程内部 2
    });

    // 2. 第一次显式 resume，启动协程并进入 lambda 运行至 yield 处挂起
    coro->resume();
    executionOrder.push_back(2);
    EXPECT_EQ(Coroutine::t_current_coroutine, nullptr);

    // 3. 第二次显式 resume，唤醒协程继续运行直至结束
    coro->resume();
    executionOrder.push_back(4);

    // 预期执行流顺序应该是: 首次运行协程内(1) -> 挂起切回主线程(2) -> 再次唤醒协程内(3) -> 协程结束切回主线程(4)
    std::vector<int> expected = {1, 2, 3, 4};
    EXPECT_EQ(executionOrder, expected);
}

} // namespace test
} // namespace rpc
} // namespace tudou
