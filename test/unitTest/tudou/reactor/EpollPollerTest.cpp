#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

#include "tudou/reactor/EventLoop.h"
#include "tudou/reactor/Channel.h"

// EpollPoller 是 EventLoop 的内部组件，不对外暴露。
// 通过 EventLoop 的公共接口间接测试 Poller 行为。
// Channel 必须在 EventLoop 线程析构，所有测试通过 run_in_loop 安全销毁。

// ───────────────────────── Channel 注册与注销 ─────────────────────────

TEST(EpollPollerTest, ChannelRegisterAndUnregisterViaEventLoop) {
    int fds[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    // 惰性注册模式下，新构造的 Channel 尚未关联任何事件，不应立刻登记在 Loop 中
    EXPECT_FALSE(loop.has_channel(ch.get()));

    ch->enable_reading();
    EXPECT_TRUE(loop.has_channel(ch.get()));

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();

    // 在 EventLoop 线程内注销 Channel（析构会调用 remove_in_register）
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

// ───────────────────────── Poll 正常返回 ─────────────────────────

TEST(EpollPollerTest, PollReturnsWhenTimerFires) {
    EventLoop loop(20);
    std::atomic<bool> timerFired{ false };

    loop.run_after(0.01, [&]() {
        timerFired = true;
        loop.quit();
    });
    loop.run_after(0.2, [&]() { loop.quit(); });
    loop.loop();

    EXPECT_TRUE(timerFired.load());
}

// ───────────────────────── 多 Channel 同时监听 ─────────────────────────

TEST(EpollPollerTest, MultipleChannelsAllReceiveEvents) {
    int fds1[2], fds2[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds1), 0);
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds2), 0);

    char c = 'x';
    (void)::write(fds1[1], &c, 1);
    (void)::write(fds2[1], &c, 1);

    EventLoop loop(20);
    std::atomic<int> readCount{ 0 };
    auto ch1 = std::make_shared<Channel>(&loop, fds1[0]);
    auto ch2 = std::make_shared<Channel>(&loop, fds2[0]);

    ch1->set_read_callback([&](Channel& c) {
        char buf[64];
        (void)::read(c.get_fd(), buf, sizeof(buf));  // 消费数据，避免 LT 重复触发
        ++readCount;
    });
    ch2->set_read_callback([&](Channel& c) {
        char buf[64];
        (void)::read(c.get_fd(), buf, sizeof(buf));
        ++readCount;
    });
    ch1->enable_reading();
    ch2->enable_reading();

    loop.run_after(0.05, [&]() {
        ch1.reset();
        ch2.reset();
        loop.quit();
    });
    loop.run_after(0.2, [&]() { ch1.reset(); ch2.reset(); loop.quit(); });
    loop.loop();

    EXPECT_EQ(readCount.load(), 2);

    ::close(fds1[0]); ::close(fds1[1]);
    ::close(fds2[0]); ::close(fds2[1]);
}