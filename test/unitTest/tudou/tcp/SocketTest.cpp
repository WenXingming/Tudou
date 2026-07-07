#include <gtest/gtest.h>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include "tudou/tcp/Socket.h"

// 辅助：创建一对已连接的 unix socket fd
static void make_socketpair(int fds[2]) {
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}

// ───────────────────────── 构造 / 析构 ─────────────────────────

TEST(SocketTest, ConstructWithValidFd) {
    int fds[2];
    make_socketpair(fds);

    Socket sock(fds[0]);
    EXPECT_EQ(sock.fd(), fds[0]);

    // 手动关闭另一端
    ::close(fds[1]);
    // sock 析构时自动关闭 fds[0]
}

TEST(SocketTest, ConstructWithInvalidFd) {
    Socket sock(-1);
    EXPECT_EQ(sock.fd(), -1);
}

TEST(SocketTest, DestructorClosesFd) {
    int fds[2];
    make_socketpair(fds);

    int fd = fds[0];
    {
        Socket sock(fd);
        EXPECT_EQ(sock.fd(), fd);
    }
    // sock 已析构，fd 应该已被关闭
    // 验证方式：对该 fd 做 fcntl 应返回 EBADF
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    ::close(fds[1]);
}

// ───────────────────────── 移动语义 ─────────────────────────

TEST(SocketTest, MoveConstructTransfersOwnership) {
    int fds[2];
    make_socketpair(fds);
    int fd = fds[0];

    Socket original(fd);
    Socket moved(std::move(original));

    EXPECT_EQ(moved.fd(), fd);
    EXPECT_EQ(original.fd(), -1);  // 被移动后变为无效

    ::close(fds[1]);
}

TEST(SocketTest, MoveAssignTransfersOwnership) {
    int fds[2];
    make_socketpair(fds);
    int fds2[2];
    make_socketpair(fds2);

    Socket a(fds[0]);
    Socket b(fds2[0]);

    int old_a_fd = a.fd();
    b = std::move(a);

    EXPECT_EQ(b.fd(), old_a_fd);
    EXPECT_EQ(a.fd(), -1);

    // fds2[0] 应该已被 b 的移动赋值关闭（b 先关闭自己的旧 fd 再接管）
    EXPECT_EQ(::fcntl(fds2[0], F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);

    ::close(fds[1]);
    ::close(fds2[1]);
}

TEST(SocketTest, MoveAssignSelfIsNoOp) {
    int fds[2];
    make_socketpair(fds);

    Socket sock(fds[0]);
    int fd = sock.fd();

    sock = std::move(sock);  // 自移动赋值
    EXPECT_EQ(sock.fd(), fd);  // fd 不应改变

    ::close(fds[1]);
}

TEST(SocketTest, MoveAssignToInvalidFdSocket) {
    int fds[2];
    make_socketpair(fds);

    Socket invalid(-1);
    Socket valid(fds[0]);
    int fd = valid.fd();

    invalid = std::move(valid);
    EXPECT_EQ(invalid.fd(), fd);
    EXPECT_EQ(valid.fd(), -1);

    ::close(fds[1]);
}

TEST(SocketTest, MoveAssignFromInvalidFdDoesNotClose) {
    int fds[2];
    make_socketpair(fds);

    Socket valid(fds[0]);
    int fd = valid.fd();

    Socket invalid(-1);
    valid = std::move(invalid);

    // valid 的旧 fd 应该已被关闭
    EXPECT_EQ(::fcntl(fd, F_GETFD), -1);
    EXPECT_EQ(valid.fd(), -1);

    ::close(fds[1]);
}