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
Buffer::Buffer(size_t initialSize)
    : buffer(kCheapPrepend + initialSize)
    , readIndex(kCheapPrepend)
    , writeIndex(kCheapPrepend) {

    assert(readable_bytes() == 0);
    assert(writable_bytes() == initialSize);
    assert(prependable_bytes() == kCheapPrepend);
}

Buffer::~Buffer() {

}

size_t Buffer::readable_bytes() const {
    return writeIndex - readIndex;
}

size_t Buffer::writable_bytes() const {
    return buffer.size() - writeIndex;
}

size_t Buffer::prependable_bytes() const {
    return readIndex - 0;
}

const char* Buffer::readable_start_ptr() const {
    return buffer.data() + readIndex;
}

// 从缓冲区读取 len 字节后维护 readIndex
void Buffer::maintain_read_index(size_t len) {
    if (len < readable_bytes()) { // 应用只读取了可读缓冲区的一部分
        readIndex += len;
    }
    else { // 应用读取了所有可读数据, 直接重置
        maintain_all_index();
    }
}

// 将读写索引重置为初始位置，清空缓冲区
void Buffer::maintain_all_index() {
    readIndex = kCheapPrepend;
    writeIndex = kCheapPrepend;
}

// 读取 len 字节为字符串并消费对应数据
std::string Buffer::read_from_buffer(size_t len) {
    if (len > readable_bytes()) {
        LOG::LOG_ERROR("Buffer::read_from_buffer(). len > readable_bytes");
    }

    std::string str(readable_start_ptr(), len);
    maintain_read_index(len);
    return std::move(str);
}

// 读取全部可读数据
std::string Buffer::read_from_buffer() {
    int readableBytes = readable_bytes();
    std::string str(read_from_buffer(readableBytes));
    maintain_read_index(readableBytes); // 其实 read_from_buffer 已经维护过了。为了保证语义清晰，再次调用
    return std::move(str);
}

// 写入原始数据到缓冲区，不够空间时扩容或整理。
void Buffer::write_to_buffer(const char* data, size_t len) {
    if (writable_bytes() < len) {
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
    if (writable_bytes() + prependable_bytes() < len + kCheapPrepend) { // 环形缓冲区
        buffer.resize(writeIndex + len);
    }
    else { // 调整缓冲区
        int readableBytes = readable_bytes();
        std::copy(buffer.begin() + readIndex, buffer.begin() + writeIndex, buffer.begin() + kCheapPrepend);
        readIndex = kCheapPrepend;
        writeIndex = readIndex + readableBytes; // 不可直接调用 readable_bytes()，正在维护 index
    }
}

/**
 * 从 fd 上读取数据，注意 events 是 LT 模式，数据没有读完也不会丢失。没有使用 while 读
 * 使用 readv，buffer 不会太小也不会太大，完美利用内存
 */
 // 从 fd 读取数据写入缓冲区的 writable 区域，并根据读取量调整索引。readBuffer 调用此函数
ssize_t Buffer::read_from_fd(int fd, int* savedErrno) {
    char extraBuf[65536];
    size_t writableBytes = writable_bytes();

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
