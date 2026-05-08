#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "tudou/tcp/EventLoop.h"

TEST(EventLoopTest, RunAfterFiresFirstScheduledTimer) {
    EventLoop loop(20);
    bool fired = false;

    loop.run_after(0.01, [&]() {
        fired = true;
        loop.quit();
        });
    loop.run_after(0.2, [&]() {
        loop.quit();
        });

    loop.loop();

    EXPECT_TRUE(fired);
}

TEST(EventLoopTest, QueueInLoopFromAnotherThreadWakesLoop) {
    EventLoop loop(20);
    std::atomic<bool> executed{ false };

    std::thread worker([&]() {
        loop.queue_in_loop([&]() {
            executed = true;
            loop.quit();
            });
        });

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();
    worker.join();

    EXPECT_TRUE(executed.load());
}

TEST(EventLoopTest, RunEveryCanCancelRepeatingTimer) {
    EventLoop loop(20);
    int count = 0;
    TimerId repeatingTimer;

    repeatingTimer = loop.run_every(0.01, [&]() {
        ++count;
        if (count == 3) {
            loop.cancel(repeatingTimer);
        }
        if (count >= 3) {
            loop.quit();
        }
        });
    loop.run_after(0.2, [&]() {
        loop.quit();
        });

    loop.loop();

    EXPECT_EQ(count, 3);
}