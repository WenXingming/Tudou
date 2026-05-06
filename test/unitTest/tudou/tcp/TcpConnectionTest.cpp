#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <memory>
#include <string>

#include "base/InetAddress.h"
#include "tudou/tcp/EventLoop.h"
#include "tudou/tcp/TcpConnection.h"

namespace {

std::shared_ptr<TcpConnection> make_connection(EventLoop& loop, int fd) {
    InetAddress localAddr("127.0.0.1", 8080);
    InetAddress peerAddr("127.0.0.1", 8081);
    return std::make_shared<TcpConnection>(&loop, fd, localAddr, peerAddr);
}

} // namespace

TEST(TcpConnectionTest, MessageCallbackCanConsumeInboundData) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    auto conn = make_connection(loop, fds[0]);
    std::string received;

    conn->set_message_callback([&](const std::shared_ptr<TcpConnection>& activeConn) {
        received = activeConn->receive();
        loop.quit();
        });
    conn->set_close_callback([](const std::shared_ptr<TcpConnection>&) {});
    conn->connection_establish();

    ASSERT_EQ(::write(fds[1], "hello", 5), 5);

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop(50);

    EXPECT_EQ(received, "hello");

    ::close(fds[1]);
}

TEST(TcpConnectionTest, SendTriggersHighWaterMarkCallbackWhenCrossingThreshold) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    auto conn = make_connection(loop, fds[0]);
    bool highWaterMarkReached = false;

    conn->set_close_callback([](const std::shared_ptr<TcpConnection>&) {});
    conn->set_high_water_mark_callback([&](const std::shared_ptr<TcpConnection>&) {
        highWaterMarkReached = true;
        }, 4);
    conn->connection_establish();

    conn->send("hello");

    EXPECT_TRUE(highWaterMarkReached);
    EXPECT_EQ(conn->get_write_buffer_size(), 5U);

    ::close(fds[1]);
}

TEST(TcpConnectionTest, HeartbeatTimeoutClosesIdleConnection) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop;
    auto conn = make_connection(loop, fds[0]);
    bool closed = false;

    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        closed = true;
        loop.quit();
        });
    conn->connection_establish();
    conn->enable_app_heartbeat(0.01, 0.02, "");

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop(20);

    EXPECT_TRUE(closed);

    ::close(fds[1]);
}