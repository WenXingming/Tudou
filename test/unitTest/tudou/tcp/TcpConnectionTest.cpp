#include <gtest/gtest.h>

#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "base/InetAddress.h"
#include "tudou/tcp/EventLoop.h"
#include "tudou/tcp/Socket.h"
#include "tudou/tcp/TcpConnection.h"

namespace {

std::shared_ptr<TcpConnection> make_connection(EventLoop& loop, int fd) {
    InetAddress localAddr("127.0.0.1", 8080);
    InetAddress peerAddr("127.0.0.1", 8081);
    return TcpConnection::create(&loop, Socket(fd), localAddr, peerAddr);
}

void fill_send_buffer_until_would_block(int fd) {
    std::string data(4096, 'x');
    while (true) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n > 0) {
            continue;
        }
        if (n == 0) {
            FAIL() << "write returned 0 while filling socket buffer";
            return;
        }
        ASSERT_TRUE(errno == EAGAIN || errno == EWOULDBLOCK) << strerror(errno);
        return;
    }
}

} // namespace

TEST(TcpConnectionTest, MessageCallbackCanConsumeInboundData) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    EventLoop loop(50);
    auto conn = make_connection(loop, fds[0]);
    std::string received;

    conn->set_message_callback([&](const std::shared_ptr<TcpConnection>& activeConn) {
        received = activeConn->receive();
        loop.quit();
        });
    conn->set_close_callback([](const std::shared_ptr<TcpConnection>&) {});

    ASSERT_EQ(::write(fds[1], "hello", 5), 5);

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    EXPECT_EQ(received, "hello");

    ::close(fds[1]); // fds[1] 未交给 Socket，需手动关闭
}

TEST(TcpConnectionTest, SendTriggersHighWaterMarkCallbackWhenCrossingThreshold) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    EventLoop loop;
    auto conn = make_connection(loop, fds[0]);
    bool highWaterMarkReached = false;

    conn->set_close_callback([](const std::shared_ptr<TcpConnection>&) {});
    conn->set_high_water_mark_callback([&](const std::shared_ptr<TcpConnection>&) {
        highWaterMarkReached = true;
        }, 4);

    fill_send_buffer_until_would_block(fds[0]);
    conn->send("hello");

    EXPECT_TRUE(highWaterMarkReached);
    EXPECT_EQ(conn->get_write_buffer_size(), 5U);

    ::close(fds[1]);
}

TEST(TcpConnectionTest, SendFromAnotherThreadIsQueuedBackToLoop) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, fds), 0);

    EventLoop loop(20);
    auto conn = make_connection(loop, fds[0]);
    std::atomic<bool> highWaterMarkReached{ false };

    conn->set_close_callback([](const std::shared_ptr<TcpConnection>&) {});
    conn->set_high_water_mark_callback([&](const std::shared_ptr<TcpConnection>&) {
        highWaterMarkReached = true;
        loop.quit();
        }, 1);

    fill_send_buffer_until_would_block(fds[0]);
    std::thread worker([conn]() {
        conn->send("hello");
        });

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();
    worker.join();

    EXPECT_TRUE(highWaterMarkReached.load());

    ::close(fds[1]);
}
