#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>
#include <memory>

#include "tudou/reactor/Channel.h"
#include "tudou/reactor/EventLoop.h"

// 辅助：创建一对已连接的 unix socket fd
static void make_socketpair(int fds[2]) {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}

// Channel 必须在 EventLoop 线程内析构（assert 守卫）。
// 所有测试用 shared_ptr<Channel> 在测试作用域持有，loop 退出后通过
// run_in_loop 在 EventLoop 线程内安全销毁。

// ───────────────────────── 基础属性 ─────────────────────────

TEST(ChannelTest, FdAndLoopAccessors) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    EXPECT_EQ(ch->get_fd(), fds[0]);
    EXPECT_EQ(ch->get_owner_loop(), &loop);

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

// ───────────────────────── 事件兴趣管理 ─────────────────────────

TEST(ChannelTest, DefaultIsNoneEvent) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    EXPECT_TRUE(ch->is_none_event());
    EXPECT_FALSE(ch->is_reading());
    EXPECT_FALSE(ch->is_writing());

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

TEST(ChannelTest, EnableAndDisableReading) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->enable_reading();
    EXPECT_TRUE(ch->is_reading());
    EXPECT_FALSE(ch->is_none_event());

    ch->disable_reading();
    EXPECT_FALSE(ch->is_reading());
    EXPECT_TRUE(ch->is_none_event());

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

TEST(ChannelTest, EnableAndDisableWriting) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->enable_writing();
    EXPECT_TRUE(ch->is_writing());
    EXPECT_FALSE(ch->is_none_event());

    ch->disable_writing();
    EXPECT_FALSE(ch->is_writing());
    EXPECT_TRUE(ch->is_none_event());

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

TEST(ChannelTest, DisableAllClearsAllInterests) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->enable_reading();
    ch->enable_writing();
    EXPECT_FALSE(ch->is_none_event());

    ch->disable_all();
    EXPECT_TRUE(ch->is_none_event());
    EXPECT_FALSE(ch->is_reading());
    EXPECT_FALSE(ch->is_writing());

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

TEST(ChannelTest, CombinedReadWriteInterest) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->enable_reading();
    ch->enable_writing();
    EXPECT_TRUE(ch->is_reading());
    EXPECT_TRUE(ch->is_writing());

    ch->disable_writing();
    EXPECT_TRUE(ch->is_reading());
    EXPECT_FALSE(ch->is_writing());

    loop.run_after(0.01, [&]() { loop.quit(); });
    loop.loop();
    loop.run_in_loop([&]() { ch.reset(); });

    ::close(fds[1]);
}

// ───────────────────────── 读回调分发 ─────────────────────────

TEST(ChannelTest, ReadCallbackFiresWhenFdIsReadable) {
    int fds[2];
    make_socketpair(fds);

    const char msg[] = "hello";
    ASSERT_EQ(::write(fds[1], msg, sizeof(msg)), static_cast<ssize_t>(sizeof(msg)));

    EventLoop loop(20);
    std::atomic<bool> readFired{ false };
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->set_read_callback([&](Channel& c) {
        EXPECT_EQ(c.get_fd(), fds[0]);
        readFired = true;
        ch.reset();   // 读完即销毁，释放 epoll 注册
        loop.quit();
    });
    ch->enable_reading();

    loop.run_after(0.2, [&]() { ch.reset(); loop.quit(); });
    loop.loop();

    EXPECT_TRUE(readFired.load());

    ::close(fds[0]);
    ::close(fds[1]);
}

// ───────────────────────── 写回调分发 ─────────────────────────

TEST(ChannelTest, WriteCallbackFiresWhenWritingEnabled) {
    int fds[2];
    make_socketpair(fds);

    EventLoop loop(20);
    std::atomic<bool> writeFired{ false };
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->set_write_callback([&](Channel& c) {
        EXPECT_EQ(c.get_fd(), fds[0]);
        writeFired = true;
        c.disable_writing();
        ch.reset();
        loop.quit();
    });
    ch->enable_writing();

    loop.run_after(0.2, [&]() { ch.reset(); loop.quit(); });
    loop.loop();

    EXPECT_TRUE(writeFired.load());

    ::close(fds[0]);
    ::close(fds[1]);
}

// ───────────────────────── 未设置回调时的行为 ─────────────────────────

TEST(ChannelTest, NoWriteCallbackSetDoesNotCrash) {
    int fds[2];
    make_socketpair(fds);

    char c = 'x';
    (void)::write(fds[1], &c, 1);

    EventLoop loop(20);
    std::atomic<bool> readFired{ false };
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->set_read_callback([&](Channel&) {
        readFired = true;
        ch.reset();
        loop.quit();
    });
    ch->enable_reading();

    loop.run_after(0.2, [&]() { ch.reset(); loop.quit(); });
    loop.loop();

    EXPECT_TRUE(readFired.load());

    ::close(fds[0]);
    ::close(fds[1]);
}

// ───────────────────────── tie 机制 ─────────────────────────

TEST(ChannelTest, TieExpiredPreventsCallbackDispatch) {
    int fds[2];
    make_socketpair(fds);

    char c = 'x';
    (void)::write(fds[1], &c, 1);

    EventLoop loop(20);
    std::atomic<bool> callbackFired{ false };
    auto ch = std::make_shared<Channel>(&loop, fds[0]);

    ch->set_read_callback([&](Channel&) {
        callbackFired = true;
    });
    ch->enable_reading();

    // 建立 tie 后立即释放 owner，使 tie_ expired
    {
        auto owner = std::make_shared<int>(42);
        ch->tie_to_object(owner);
    }

    // 定时器回调在 Channel 存活期间验证结果，然后安全销毁 Channel。
    // 此时 epoll 已经返回过 EPOLLIN（tie expired → 回调被跳过），callbackFired 仍为 false。
    loop.run_after(0.05, [&]() {
        EXPECT_FALSE(callbackFired.load());
        ch.reset();   // Channel 存活期内销毁，epoll 注销在析构中完成
        loop.quit();
    });
    loop.run_after(0.2, [&]() { ch.reset(); loop.quit(); });
    loop.loop();

    // Channel 已在定时器回调中销毁，fd 已注销 epoll，可安全关闭。
    ::close(fds[0]);
    ::close(fds[1]);
}