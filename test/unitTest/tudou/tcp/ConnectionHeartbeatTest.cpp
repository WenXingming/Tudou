#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <memory>

#include "base/InetAddress.h"
#include "tudou/tcp/ConnectionHeartbeat.h"
#include "tudou/tcp/EventLoop.h"
#include "tudou/tcp/Socket.h"
#include "tudou/tcp/TcpConnection.h"

namespace {

std::shared_ptr<TcpConnection> make_connection(EventLoop& loop, int fd) {
    InetAddress localAddr("127.0.0.1", 8080);
    InetAddress peerAddr("127.0.0.1", 8081);
    return std::make_shared<TcpConnection>(&loop, Socket(fd), localAddr, peerAddr);
}

} // namespace

TEST(ConnectionHeartbeatTest, TimeoutClosesIdleConnection) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    auto conn = make_connection(loop, fds[0]);
    bool closed = false;

    conn->set_message_callback([](const std::shared_ptr<TcpConnection>&) {});
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        closed = true;
        loop.quit();
        });
    conn->tie_to_object(conn);

    auto heartbeat = std::make_shared<ConnectionHeartbeat>(conn, 0.01, 0.02);
    heartbeat->start();

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    EXPECT_TRUE(closed);

    ::close(fds[1]);
}

TEST(ConnectionHeartbeatTest, RefreshExtendsIdleDeadline) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    EventLoop loop(20);
    auto conn = make_connection(loop, fds[0]);
    bool closed = false;

    conn->set_message_callback([](const std::shared_ptr<TcpConnection>&) {});
    conn->set_close_callback([&](const std::shared_ptr<TcpConnection>&) {
        closed = true;
        loop.quit();
        });
    conn->tie_to_object(conn);

    auto heartbeat = std::make_shared<ConnectionHeartbeat>(conn, 0.01, 0.03);
    heartbeat->start();

    loop.run_after(0.015, [heartbeat]() {
        heartbeat->refresh();
        });
    loop.run_after(0.035, [&]() {
        heartbeat->stop();
        loop.quit();
        });
    loop.loop();

    EXPECT_FALSE(closed);

    ::close(fds[1]);
}