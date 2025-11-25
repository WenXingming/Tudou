/**
 * @file Buffer.h
 * @brief 高效字节缓冲区，管理可读/可写/预留区域，并支持与 fd 的非阻塞读写。
 * @author
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

#include "../base/Log.h"

 // 构造缓冲区，预留 prepend 区域并初始化读写索引。
Buffer::Buffer(size_t initSize)
    : buffer(kCheapPrepend + kInitialSize)
    , readIndex(kCheapPrepend)
    , writeIndex(kCheapPrepend) {}

Buffer::~Buffer() {}

// 返回当前可读字节数（readable 区域长度）。
size_t Buffer::readable_bytes_size() const {
    return writeIndex - readIndex;
}

// 返回当前可写字节数（writable 区域长度）。
size_t Buffer::writable_bytes_size() const {
    return buffer.size() - writeIndex;
}

// 返回预留区域大小（prependable 区域长度）。
size_t Buffer::prependable_bytes_size() const {
    return readIndex - 0;
}

// 获取 readable 区域起始指针。
const char* Buffer::readable_start_ptr() const {
    return buffer.data() + readIndex;
}

// 从缓冲区读取 len 字节后推进 readIndex；若读完则重置。
void Buffer::retrieve_readIndex(size_t len) {
    if (len < readable_bytes_size()) { // 应用只读取了可读缓冲区的一部分
        readIndex += len;
    }
    else {
        retrieve_all_index();
    }
}

// 将读写索引重置为初始位置，清空缓冲区。
void Buffer::retrieve_all_index() {
    readIndex = kCheapPrepend;
    writeIndex = kCheapPrepend;
}

// 读取 len 字节为字符串并消费对应数据。
std::string Buffer::retrieve_as_string(size_t len) {
    if (len > readable_bytes_size()) {
        LOG::LOG_ERROR("Buffer::retrieve_as_string(). len > readable_bytes_size");
    }

    len = std::min(len, readable_bytes_size());
    std::string str(readable_start_ptr(), len);
    retrieve_readIndex(len);
    return std::move(str);
}

// 读取全部可读数据为字符串并清空。
std::string Buffer::retrieve_all_as_string() {
    int readableBytes = readable_bytes_size();
    return retrieve_as_string(readableBytes);
}

// 读取全部可读数据为字符串但不更新索引。
std::string Buffer::read_from_buffer() {
    auto readablePtr = readable_start_ptr();
    auto readableBytes = readable_bytes_size();
    std::string str(readablePtr, readableBytes);
    return std::move(str);
}

// 写入原始数据到缓冲区，不够空间时扩容或整理。
void Buffer::write_to_buffer(const char* data, size_t len) {
    if (len > writable_bytes_size()) {
        make_space(len); // 环形缓冲区：要么扩容要么调整
    }
    std::copy(data, data + len, buffer.begin() + writeIndex);
    writeIndex += len;
}

// 写入 std::string 数据到缓冲区。
void Buffer::write_to_buffer(const std::string& str) {
    write_to_buffer(str.data(), str.size());
}

// 通过搬移或扩容确保有 len 字节可写空间。
void Buffer::make_space(size_t len) {
    if (writable_bytes_size() + prependable_bytes_size() - kCheapPrepend < len) { // 环形缓冲区
        buffer.resize(writeIndex + len);
    }
    else { // 调整缓冲区
        int readableBytes = readable_bytes_size();
        std::copy(buffer.begin() + readIndex, buffer.begin() + writeIndex, buffer.begin() + kCheapPrepend);
        readIndex = kCheapPrepend;
        writeIndex = readIndex + readableBytes /* readable_bytes_size() */; // 不可直接调用
    }
}

/**
 * 从 fd 上读取数据，注意 events 是 LT 模式，数据没有读完也不会丢失。没有使用 while 读
 * 使用 readv，buffer 不会太小也不会太大，完美利用内存
 */
// 从 fd 读取数据写入缓冲区的 writable 区域，并根据读取量调整索引。readBuffer 调用此函数
ssize_t Buffer::read_from_fd(int fd, int* savedErrno) {
    char extraBuf[65536];
    size_t writableBytes = writable_bytes_size();

    struct iovec vec[2];
    vec[0].iov_base = buffer.data() + writeIndex;
    vec[0].iov_len = writableBytes;
    vec[1].iov_base = extraBuf;
    vec[1].iov_len = sizeof(extraBuf);

    const int cnt = (writableBytes < sizeof(extraBuf)) ? 2 : 1;
    ssize_t n = ::readv(fd, vec, cnt);
    if (n < 0) {
        *savedErrno = errno;
    }
    else if (static_cast<size_t>(n) <= writableBytes) {
        writeIndex += n;
    }
    else {
        writeIndex = buffer.size();
        write_to_buffer(extraBuf, n - writableBytes);
    }
    return n;

    /// TODO: 设置 fd 为非阻塞，然后循环接受到 buffer
}

// 将缓冲区可读数据写入 fd，并根据写入量调整索引。writeBuffer 调用此函数
ssize_t Buffer::write_to_fd(int fd, int* savedErrno) {
    auto readablePtr = readable_start_ptr();
    auto readableBytes = readable_bytes_size();
    ssize_t n = ::write(fd, readablePtr, readableBytes);
    if (n < 0) {
        *savedErrno = errno;
    }
    else {
        retrieve_readIndex(n);
    }
    return n;
}
