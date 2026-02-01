/**
 * @file Buffer.cpp
 * @brief 高效字节缓冲区，管理可读/可写/预留区域，并支持与 fd 的非阻塞读写。
 * @author WenXingming
 * @project: https://github.com/WenXingming/tudou
 *
 */

#include "Buffer.h"
#include <unistd.h>
#include <errno.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <stdio.h>
#include <cassert>
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
    // 读取 len 字节为字符串并消费对应数据
    if (len > readable_bytes()) {
        spdlog::error("Buffer::read_from_buffer(). len > readable_bytes");
    }

    const char* start = readable_start_ptr();
    std::string str(start, len);
    maintain_read_index(len);
    return std::move(str);
}

std::string Buffer::read_from_buffer() {
    // 读取全部可读数据
    int readableBytes = readable_bytes();
    std::string str = read_from_buffer(readableBytes);
    return std::move(str);
}

void Buffer::write_to_buffer(const char* data, size_t len) {
    // 写入原始数据到缓冲区，不够空间时扩容或整理。
    if (writable_bytes() < len) {
        make_space(len); // 环形缓冲区：要么扩容要么调整
    }
    std::copy(data, data + len, buffer_.begin() + writeIndex_);
    writeIndex_ += len;
}

void Buffer::write_to_buffer(const std::string& str) {
    // 写入 std::string 数据到缓冲区。
    write_to_buffer(str.data(), str.size());
}

ssize_t Buffer::read_from_fd(int fd, int* savedErrno) {
    // 从 fd 上读取数据，注意 events 是 LT 模式，数据没有读完也不会丢失。没有使用 while 读
    // 使用 readv，buffer 不会太小也不会太大，完美利用内存
    // TODO: 设置 fd 为非阻塞，然后循环接受到 buffer

    char extraBuf[65536];
    size_t writableBytes = writable_bytes();

    struct iovec vec[2];
    vec[0].iov_base = buffer_.data() + writeIndex_;
    vec[0].iov_len = writableBytes;
    vec[1].iov_base = extraBuf;
    vec[1].iov_len = sizeof(extraBuf);

    const int cnt = (writableBytes < sizeof(extraBuf)) ? 2 : 1;
    ssize_t n = ::readv(fd, vec, cnt);
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
    // 将缓冲区可读数据写入 fd，并根据写入量调整索引。writeBuffer 调用此函数
    auto readablePtr = readable_start_ptr();
    auto readableBytes = readable_bytes();
    ssize_t n = ::write(fd, readablePtr, readableBytes);
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
    return readIndex_ - 0;
}

const char* Buffer::readable_start_ptr() const {
    return buffer_.data() + readIndex_;
}

void Buffer::maintain_read_index(size_t len) {
    // 从缓冲区读取 len 字节后维护 readIndex
    // 应用只读取了可读缓冲区的一部分
    if (len < readable_bytes()) {
        readIndex_ += len;
        return;
    }
    // 应用读取了所有可读数据, 直接重置
    maintain_all_index();
}

void Buffer::maintain_all_index() {
    // 将读写索引重置为初始位置，清空缓冲区
    readIndex_ = kCheapPrepend_;
    writeIndex_ = kCheapPrepend_;
}

void Buffer::make_space(size_t len) {
    // 通过搬移或扩容确保有 len 字节可写空间。
    // 判断是否通过搬移数据可以满足需求（环形缓冲区），若不行则扩容
    if (writable_bytes() + prependable_bytes() < len + kCheapPrepend_) {
        buffer_.resize(writeIndex_ + len);
        return;
    }
    // 若搬移数据可以满足需求，则通过搬移数据调整缓冲区
    int readableBytes = readable_bytes();
    std::copy(buffer_.begin() + readIndex_, buffer_.begin() + writeIndex_, buffer_.begin() + kCheapPrepend_);
    readIndex_ = kCheapPrepend_;
    writeIndex_ = readIndex_ + readableBytes; // 不可直接调用 readable_bytes()，正在维护 index
}
