#include <gtest/gtest.h>

#include "tudou/tcp/EventLoopThreadPool.h"

TEST(EventLoopThreadPoolTest, GetNextLoopFallsBackToMainLoopWhenNoIoThreadsExist) {
    EventLoopThreadPool pool("test", 0);

    pool.start();

    ASSERT_NE(pool.get_main_loop(), nullptr);
    EXPECT_EQ(pool.get_next_loop(), pool.get_main_loop());

    const auto loops = pool.get_all_loops();
    ASSERT_EQ(loops.size(), 1U);
    EXPECT_EQ(loops.front(), pool.get_main_loop());
}

TEST(EventLoopThreadPoolTest, GetNextLoopRoundRobinsAcrossIoLoops) {
    EventLoopThreadPool pool("test", 2);

    pool.start();

    EventLoop* firstLoop = pool.get_next_loop();
    EventLoop* secondLoop = pool.get_next_loop();
    EventLoop* thirdLoop = pool.get_next_loop();

    ASSERT_NE(pool.get_main_loop(), nullptr);
    ASSERT_NE(firstLoop, nullptr);
    ASSERT_NE(secondLoop, nullptr);
    EXPECT_NE(firstLoop, pool.get_main_loop());
    EXPECT_NE(secondLoop, pool.get_main_loop());
    EXPECT_NE(firstLoop, secondLoop);
    EXPECT_EQ(firstLoop, thirdLoop);
}