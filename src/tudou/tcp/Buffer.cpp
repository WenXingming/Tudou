// ============================================================================
// Buffer.cpp
// Buffer 的实现只关注字节搬运、空间回收和系统调用衔接，不承担任何业务含义。
// ============================================================================

#include "Buffer.h"

#include <algorithm>
#include <errno.h>
#include <cassert>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "spdlog/spdlog.h"

const size_t Buffer::kCheapPrepend_ = 8;
const size_t Buffer::kInitialSize_ = 1024;

// 构造缓冲区，预留 prepend 区域并初始化读写索引。
Buffer::Buffer(size_t initialSize) :
    buffer_(kCheapPrepend_ + initialSize),
    readIndex_(kCheapPrepend_),
    writeIndex_(kCheapPrepend_) {

    assert(readable_bytes() == 0);
    assert(writable_bytes() == initialSize);
    assert(prependable_bytes() == kCheapPrepend_);
}

Buffer::~Buffer() {

}

std::string Buffer::read_from_buffer(size_t len) {
    const size_t readableBytes = readable_bytes();
    if (len > readableBytes) {
        spdlog::warn("Buffer::read_from_buffer(). len {} > readable_bytes {}, truncating.", len, readableBytes);
        len = readableBytes;
    }

    const char* start = readable_start_ptr();
    std::string str(start, len);
    maintain_read_index(len);
    return str;
}

std::string Buffer::read_from_buffer() {
    return read_from_buffer(readable_bytes());
}

void Buffer::write_to_buffer(const char* data, size_t len) {
    // 先保证空间，再顺序追加数据，避免调用者关注底层搬移或扩容细节。
    if (writable_bytes() < len) {
        make_space(len);
    }
    std::copy(data, data + len, buffer_.begin() + writeIndex_);
    writeIndex_ += len;
}

void Buffer::write_to_buffer(const std::string& str) {
    // 写入 std::string 数据到缓冲区。
    write_to_buffer(str.data(), str.size());
}

ssize_t Buffer::read_from_fd(int fd, int* savedErrno) {
    // 使用 readv 让主缓冲与临时栈缓冲协作，减少“先扩容再读”的额外内存动作。
    char extraBuf[65536];
    const size_t writableBytes = writable_bytes();

    struct iovec vec[2];
    vec[0].iov_base = buffer_.data() + writeIndex_;
    vec[0].iov_len = writableBytes;
    vec[1].iov_base = extraBuf;
    vec[1].iov_len = sizeof(extraBuf);

    const int cnt = (writableBytes < sizeof(extraBuf)) ? 2 : 1;
    const ssize_t n = ::readv(fd, vec, cnt);
    if (n < 0) {
        *savedErrno = errno;
    }
    else if (static_cast<size_t>(n) <= writableBytes) {
        writeIndex_ += n;
    }
    else {
        writeIndex_ = buffer_.size();
        write_to_buffer(extraBuf, n - writableBytes);
    }
    return n;
}

ssize_t Buffer::write_to_fd(int fd, int* savedErrno) {
    const char* readablePtr = readable_start_ptr();
    const size_t readableBytes = readable_bytes();
    const ssize_t n = ::write(fd, readablePtr, readableBytes);
    if (n < 0) {
        *savedErrno = errno;
    }
    else {
        maintain_read_index(n);
    }
    return n;
}

size_t Buffer::readable_bytes() const {
    return writeIndex_ - readIndex_;
}

size_t Buffer::writable_bytes() const {
    return buffer_.size() - writeIndex_;
}

size_t Buffer::prependable_bytes() const {
    return readIndex_;
}

const char* Buffer::readable_start_ptr() const {
    return buffer_.data() + readIndex_;
}

void Buffer::maintain_read_index(size_t len) {
    // 只消费部分数据时前移读指针；消费完则整体复位，避免碎片长期累积。
    if (len < readable_bytes()) {
        readIndex_ += len;
        return;
    }
    maintain_all_index();
}

void Buffer::maintain_all_index() {
    readIndex_ = kCheapPrepend_;
    writeIndex_ = kCheapPrepend_;
}

void Buffer::make_space(size_t len) {
    // 优先复用前部空洞；只有复用后仍不够时才扩容，避免高频小对象重分配。
    if (writable_bytes() + prependable_bytes() < len + kCheapPrepend_) {
        buffer_.resize(writeIndex_ + len);
        return;
    }

    const size_t readableBytes = readable_bytes();
    std::copy(buffer_.begin() + readIndex_, buffer_.begin() + writeIndex_, buffer_.begin() + kCheapPrepend_);
    readIndex_ = kCheapPrepend_;
    writeIndex_ = readIndex_ + readableBytes;
}
