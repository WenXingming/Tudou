#include <gtest/gtest.h>

#include <sys/socket.h>
#include <unistd.h>

#include <string>

#include "tudou/tcp/Buffer.h"

TEST(BufferTest, ReadFromBufferTruncatesToReadableBytes) {
    Buffer buffer;

    buffer.write_to_buffer("hello");

    EXPECT_EQ(buffer.read_from_buffer(32), "hello");
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST(BufferTest, WriteToBufferReusesPrependableSpaceWithoutDataLoss) {
    Buffer buffer(8);

    buffer.write_to_buffer("abcdef");
    EXPECT_EQ(buffer.read_from_buffer(4), "abcd");

    buffer.write_to_buffer("ghijkl");

    EXPECT_EQ(buffer.read_from_buffer(), "efghijkl");
    EXPECT_EQ(buffer.readable_bytes(), 0U);
}

TEST(BufferTest, ReadFromFdUsesExtraBufferWhenWritableSpaceIsInsufficient) {
    int fds[2] = { -1, -1 };
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);

    Buffer buffer(8);
    const std::string payload(64, 'x');
    ASSERT_EQ(::write(fds[1], payload.data(), payload.size()), static_cast<ssize_t>(payload.size()));

    int savedErrno = 0;
    EXPECT_EQ(buffer.read_from_fd(fds[0], &savedErrno), static_cast<ssize_t>(payload.size()));
    EXPECT_EQ(buffer.readable_bytes(), payload.size());
    EXPECT_EQ(buffer.read_from_buffer(), payload);

    ::close(fds[0]);
    ::close(fds[1]);
}