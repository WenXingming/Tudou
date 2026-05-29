#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "base/InetAddress.h"
#include "tudou/tcp/Acceptor.h"
#include "tudou/reactor/EventLoop.h"
#include "tudou/net/Socket.h"

namespace {

sockaddr_in read_bound_address(int fd) {
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    EXPECT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length), 0);
    return address;
}

} // namespace

TEST(AcceptorTest, AcceptPublishesNonBlockingCloseOnExecSocket) {
    EventLoop loop(20);
    Acceptor acceptor(&loop, InetAddress("127.0.0.1", 0));

    int acceptedFd = -1;
    std::string peerIp;
    Socket acceptedSocket(-1); // 接收回调转移的 Socket 所有权，延长 fd 生命周期

    acceptor.set_connect_callback([&](Socket connSocket, const InetAddress& peerAddr) {
        acceptedFd = connSocket.fd();
        acceptedSocket = std::move(connSocket); // 接管所有权，避免回调结束后 fd 被关闭
        peerIp = peerAddr.get_ip();
        loop.quit();
        });

    const sockaddr_in listenAddress = read_bound_address(acceptor.get_listen_fd());

    const int clientFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
    ASSERT_GE(clientFd, 0);
    ASSERT_EQ(::connect(clientFd, reinterpret_cast<const sockaddr*>(&listenAddress), sizeof(listenAddress)), 0);

    loop.run_after(0.2, [&]() {
        loop.quit();
        });
    loop.loop();

    ASSERT_GE(acceptedFd, 0);
    EXPECT_EQ(peerIp, "127.0.0.1");

    const int statusFlags = ::fcntl(acceptedFd, F_GETFL, 0);
    ASSERT_GE(statusFlags, 0);
    EXPECT_NE(statusFlags & O_NONBLOCK, 0);

    const int descriptorFlags = ::fcntl(acceptedFd, F_GETFD, 0);
    ASSERT_GE(descriptorFlags, 0);
    EXPECT_NE(descriptorFlags & FD_CLOEXEC, 0);

    ::close(clientFd);
    // acceptedSocket 析构时自动关闭 acceptedFd，无需手动 ::close(acceptedFd)
}
