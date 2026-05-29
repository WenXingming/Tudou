#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "tudou/reactor/EventLoop.h"

// TimerQueue 不对外暴露，通过 EventLoop 的公共定时器接口间接测试。

// ───────────────────────── 一次性定时器 ─────────────────────────

TEST(TimerQueueTest, OneShotTimerFiresOnce) {
    EventLoop loop(20);
    int count = 0;

    loop.run_after(0.01, [&]() {
        ++count;
        loop.quit();
    });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_EQ(count, 1);
}

TEST(TimerQueueTest, OneShotTimerDoesNotFireAgain) {
    EventLoop loop(20);
    std::atomic<int> count{ 0 };

    loop.run_after(0.01, [&]() {
        ++count;
    });
    // 足够长时间后检查，确认只触发一次
    loop.run_after(0.15, [&]() {
        loop.quit();
    });
    loop.run_after(0.3, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_EQ(count.load(), 1);
}

// ───────────────────────── 周期定时器 ─────────────────────────

TEST(TimerQueueTest, RepeatingTimerFiresMultipleTimes) {
    EventLoop loop(20);
    std::atomic<int> count{ 0 };

    loop.run_every(0.02, [&]() {
        ++count;
    });
    // 足够长的时间后检查
    loop.run_after(0.12, [&]() { loop.quit(); });
    loop.run_after(0.3, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_GE(count.load(), 3);
}

TEST(TimerQueueTest, RepeatingTimerCanBeCancelled) {
    EventLoop loop(20);
    std::atomic<int> count{ 0 };
    TimerId repeatingId;

    repeatingId = loop.run_every(0.02, [&]() {
        ++count;
        if (count.load() == 3) {
            loop.cancel(repeatingId);
        }
    });
    loop.run_after(0.15, [&]() { loop.quit(); });
    loop.run_after(0.3, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_EQ(count.load(), 3);
}

// ───────────────────────── 取消定时器 ─────────────────────────

TEST(TimerQueueTest, CancelBeforeFirePreventsCallback) {
    EventLoop loop(20);
    std::atomic<bool> fired{ false };

    TimerId id = loop.run_after(0.1, [&]() {
        fired = true;
    });
    // 立即取消
    loop.cancel(id);

    loop.run_after(0.05, [&]() {
        // 定时器还没到期，检查它是否已被取消
    });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(fired.load());
}

TEST(TimerQueueTest, CancelNonExistentTimerDoesNotCrash) {
    EventLoop loop(20);

    // 取消一个从未注册过的 TimerId
    loop.cancel(TimerId(99999));

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();
    // 不崩溃即通过
}

TEST(TimerQueueTest, CancelTwiceDoesNotCrash) {
    EventLoop loop(20);

    TimerId id = loop.run_after(0.1, []() {});
    loop.cancel(id);
    loop.cancel(id);  // 第二次取消，不应崩溃

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();
}

// ───────────────────────── 多定时器并发 ─────────────────────────

TEST(TimerQueueTest, MultipleTimersWithDifferentDelays) {
    EventLoop loop(20);
    std::vector<int> order;

    loop.run_after(0.03, [&]() { order.push_back(2); });
    loop.run_after(0.01, [&]() { order.push_back(1); });
    loop.run_after(0.05, [&]() { order.push_back(3); });

    loop.run_after(0.1, [&]() { loop.quit(); });
    loop.run_after(0.3, [&]() { loop.quit(); });
    loop.loop();

    ASSERT_EQ(order.size(), 3U);
    // 应按到期时间顺序执行
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
    EXPECT_EQ(order[2], 3);
}

TEST(TimerQueueTest, ManyTimersAllFire) {
    EventLoop loop(20);
    std::atomic<int> count{ 0 };
    constexpr int N = 50;

    for (int i = 0; i < N; ++i) {
        loop.run_after(0.01, [&]() { ++count; });
    }
    loop.run_after(0.1, [&]() { loop.quit(); });
    loop.run_after(0.3, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_EQ(count.load(), N);
}

// ───────────────────────── 回调中取消自身 ─────────────────────────

TEST(TimerQueueTest, CancelRepeatingTimerFromItsOwnCallback) {
    EventLoop loop(20);
    std::atomic<int> count{ 0 };
    TimerId id;

    id = loop.run_every(0.02, [&]() {
        ++count;
        if (count.load() >= 3) {
            loop.cancel(id);
        }
    });
    loop.run_after(0.15, [&]() { loop.quit(); });
    loop.run_after(0.3, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_EQ(count.load(), 3);
}

// ───────────────────────── 回调中添加新定时器 ─────────────────────────

TEST(TimerQueueTest, AddTimerFromWithinCallback) {
    EventLoop loop(20);
    std::atomic<bool> secondFired{ false };

    loop.run_after(0.01, [&]() {
        // 在第一个定时器的回调中添加第二个
        loop.run_after(0.01, [&]() {
            secondFired = true;
            loop.quit();
        });
    });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(secondFired.load());
}

// ───────────────────────── run_at 接口 ─────────────────────────

TEST(TimerQueueTest, RunAtFiresAtSpecifiedTime) {
    EventLoop loop(20);
    std::atomic<bool> fired{ false };

    auto when = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
    loop.run_at(when, [&]() {
        fired = true;
        loop.quit();
    });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(fired.load());
}

// ───────────────────────── 跨线程添加定时器 ─────────────────────────

TEST(TimerQueueTest, AddTimerFromAnotherThread) {
    EventLoop loop(20);
    std::atomic<bool> fired{ false };

    std::thread worker([&]() {
        loop.run_after(0.01, [&]() {
            fired = true;
            loop.quit();
        });
    });

    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();
    worker.join();

    EXPECT_TRUE(fired.load());
}

TEST(TimerQueueTest, CancelFromAnotherThread) {
    EventLoop loop(20);
    std::atomic<bool> fired{ false };

    TimerId id = loop.run_after(0.1, [&]() {
        fired = true;
    });

    std::thread worker([&]() {
        loop.cancel(id);
    });
    worker.join();

    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_FALSE(fired.load());
}