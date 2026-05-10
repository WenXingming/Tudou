#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "tudou/tcp/EventLoop.h"
#include "tudou/tcp/TcpConnection.h"
#include "tudou/tcp/TcpServer.h"

namespace {

uint16_t reserve_free_port() {
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    if (fd < 0) {
        return 0;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1
        || ::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        return 0;
    }

    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        ::close(fd);
        return 0;
    }

    if (::close(fd) != 0) {
        return 0;
    }

    return ntohs(addr.sin_port);
}

int connect_with_retry(uint16_t port) {
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr) != 1) {
        return -1;
    }

    for (int retry = 0; retry < 200; ++retry) {
        const int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
        if (clientFd < 0) {
            return -1;
        }

        if (::connect(clientFd, reinterpret_cast<const sockaddr*>(&serverAddr), sizeof(serverAddr)) == 0) {
            return clientFd;
        }

        ::close(clientFd);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return -1;
}

} // namespace

TEST(TcpServerTest, ConnectionLifecycleCallbacksFireInSingleThreadMode) {
    const uint16_t port = reserve_free_port();
    ASSERT_NE(port, 0);

    TcpServer server("127.0.0.1", port, 0);
    std::atomic<bool> connected{ false };
    std::atomic<bool> closed{ false };

    server.set_connection_callback([&](const std::shared_ptr<TcpConnection>& conn) {
        connected = true;
        conn->get_loop()->run_after(0.2, [loop = conn->get_loop()]() {
            loop->quit();
            });
        });
    server.set_close_callback([&](const std::shared_ptr<TcpConnection>& conn) {
        closed = true;
        conn->get_loop()->quit();
        });

    std::thread serverThread([&]() {
        server.start();
        });

    const int clientFd = connect_with_retry(port);
    ASSERT_GE(clientFd, 0);
    ASSERT_EQ(::close(clientFd), 0);

    serverThread.join();

    EXPECT_TRUE(connected.load());
    EXPECT_TRUE(closed.load());
}