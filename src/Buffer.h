/**
 * @file Buffer.h
 * @brief 高效字节缓冲区，管理可读/可写/预留区域，并支持与 fd 的非阻塞读写。
 * @author
 * @project: https://github.com/WenXingming/tudou
 * @reference: https://www.bilibili.com/video/BV1PS4y1D74z/?spm_id_from=333.337.search-card.all.click&vd_source=5f255b90a5964db3d7f44633d085b6e4
 * @details
 *
 * 内部模型：
 *   A buffer class modeled after org.jboss.netty.buffer.ChannelBuffer
 *   设计参考 Netty 的 ChannelBuffer
 *   @code
 *   +-------------------+------------------+------------------+
 *   | prependable bytes |  readable bytes  |  writable bytes  |
 *   |                   |     (CONTENT)    |                  |
 *   +-------------------+------------------+------------------+
 *   |                   |                  |                  |
 *   0      <=      readerIndex   <=   writerIndex     <=     size
 *   @endcode
 *
 * - 共有两个数据流向（缓冲区设计的精巧之处在于读、写空间的自动转换）：
 *   1. 从 fd 读取数据写入 InputBuffer 的 writable bytes 区域（read_from_fd），自动变成可读区域，供上层从缓冲区读取使用（read_from_buffer）
 *   2. 上层写入数据到 OutputBuffer 的 writable bytes 区域（write_to_buffer），自动变成可读区域，供写入 fd 使用（write_to_fd）
 * - 总之：上层通过 read_from_buffer()/write_to_buffer() 与缓冲区进行数据搬运，缓冲区通过 read_from_fd()/write_to_fd() 与 fd 进行数据搬运。
 *
 *
 * 线程模型：
 * - 非线程安全；通常在所属连接的 EventLoop 线程内使用，跨线程需外部同步。
 *
 */

#pragma once
#include <vector>
#include <string>

class Buffer {
private:
    static const size_t kCheapPrepend = 8;
    static const size_t kInitialSize = 1024;

    std::vector<char> buffer;
    size_t readIndex;
    size_t writeIndex;

private:
    void make_space(size_t len);

public:
    explicit Buffer(size_t initialSize = kInitialSize);
    ~Buffer();

    size_t prependable_bytes() const;
    size_t readable_bytes() const;
    size_t writable_bytes() const;

    const char* readable_start_ptr() const;

    void maintain_read_index(size_t len);   // 读走 len 个字节，维护 index
    void maintain_all_index();  // 读走所有字节，维护 index

    std::string read_from_buffer(size_t len);              // 读走 len 个字节
    std::string read_from_buffer();
    void write_to_buffer(const char* data, size_t len);
    void write_to_buffer(const std::string& str);

    // 提供给 TcpConnection （回调函数）使用的接口。内部实现调用 write_to_buffer/read_from_buffer
    ssize_t read_from_fd(int fd, int* savedErrno); // fd ==> buffer: 从 fd 读数据写入 buffer 的 writable 区域（写入 buffer）
    ssize_t write_to_fd(int fd, int* savedErrno);  // buffer ==> fd: 把 buffer 的 readable 区域写入 fd（从 buffer 读出）
};
