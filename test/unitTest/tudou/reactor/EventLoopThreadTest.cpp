#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "tudou/reactor/EventLoopThread.h"
#include "tudou/reactor/EventLoop.h"

// ───────────────────────── 基础生命周期 ─────────────────────────

TEST(EventLoopThreadTest, GetLoopReturnsNonNullAfterConstruction) {
    EventLoopThread elt;
    EXPECT_NE(elt.get_loop(), nullptr);
}

TEST(EventLoopThreadTest, DestructorJoinsThreadCleanly) {
    // 构造后立即析构，不应崩溃或死锁
    {
        EventLoopThread elt;
        EXPECT_NE(elt.get_loop(), nullptr);
    }
    // elt 已析构，线程已 join
}

TEST(EventLoopThreadTest, LoopRunsInDifferentThread) {
    EventLoopThread elt;
    auto* loop = elt.get_loop();
    ASSERT_NE(loop, nullptr);

    EXPECT_FALSE(loop->is_in_loop_thread());
}

// ───────────────────────── 初始化回调 ─────────────────────────

TEST(EventLoopThreadTest, InitCallbackIsCalled) {
    std::atomic<bool> initCalled{ false };
    EventLoop* capturedLoop = nullptr;

    EventLoopThread elt([&](EventLoop* loop) {
        initCalled = true;
        capturedLoop = loop;
    });

    EXPECT_TRUE(initCalled.load());
    EXPECT_EQ(capturedLoop, elt.get_loop());
}

TEST(EventLoopThreadTest, InitCallbackRunsInLoopThread) {
    std::atomic<bool> calledFromLoopThread{ false };

    EventLoopThread elt([&](EventLoop* loop) {
        calledFromLoopThread = loop->is_in_loop_thread();
    });

    EXPECT_TRUE(calledFromLoopThread.load());
}

// ───────────────────────── 跨线程投递 ─────────────────────────

TEST(EventLoopThreadTest, RunInLoopFromAnotherThreadExecutes) {
    EventLoopThread elt;
    auto* loop = elt.get_loop();
    ASSERT_NE(loop, nullptr);

    std::atomic<bool> executed{ false };
    loop->run_in_loop([&]() {
        executed = true;
    });

    // 等待执行完成
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_TRUE(executed.load());
}

// ───────────────────────── 多次构造析构 ─────────────────────────

TEST(EventLoopThreadTest, MultipleConstructDestructCycles) {
    for (int i = 0; i < 3; ++i) {
        EventLoopThread elt;
        EXPECT_NE(elt.get_loop(), nullptr);
    }
}