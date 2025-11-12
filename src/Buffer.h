/**
 * @file Buffer.h
 * @brief 高效字节缓冲区，管理可读/可写/预留区域，并支持与 fd 的非阻塞读写。
 * @author
 * @project: https://github.com/WenXingming/tudou
 * @details
 *
 * 内部模型：
 *   A buffer class modeled after org.jboss.netty.buffer.ChannelBuffer
 *   @code
 *   +-------------------+------------------+------------------+
 *   | prependable bytes |  readable bytes  |  writable bytes  |
 *   |                   |     (CONTENT)    |                  |
 *   +-------------------+------------------+------------------+
 *   |                   |                  |                  |
 *   0      <=      readerIndex   <=   writerIndex     <=     size
 *   @endcode
 *
 * - 写入后自动成为可读数据（writerIndex 前移）；读取仅推进 readerIndex；retrieve_all 可重置。
 * - make_space 在可写空间不足时优先搬移复用，否则扩容，减少分配与拷贝。
 * - 设计的精巧之地在于：
 * -    1. 读取 fd 写入 buffer（当然是写入 writable bytes）后的数据对于上层而言自动转换为了可读这一角色；
 *         例如：fd ==> buffer 后，数据转变为了可读区域，上层只需从 readBuffer 的可读区域取数据
 * -    2. 从 buffer（当然是 readable bytes）中读取数据到 fd 后，数据对于上层而言自动转变为了可写区域。
 *         上层 ==> buffer 后，数据转变为了可读区域，write_to_fd 也只需从 writeBuffer 的可读区域取数据
 *
 * I/O 约定：
 * - read_from_fd(): 将 fd 数据读入 writable 区域；返回读取字节数，失败时设置 savedErrno（如 EAGAIN）。
 * - write_to_fd(): 将 readable 区域写入 fd；返回写入字节数，失败时设置 savedErrno（如 EAGAIN）。
 * - 上层通过 write_to_buffer()/read_from_buffer() 与缓冲区进行数据搬运。
 *
 * 线程模型：
 * - 非线程安全；通常在所属连接的 EventLoop 线程内使用，跨线程需外部同步。
 *
 * 参考：
 * - 设计参考 Netty 的 ChannelBuffer。
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

    size_t prependable_bytes_size() const;

    const char* readable_start_ptr() const;       // 读指针
    void retrieve_readIndex(size_t len);              // 维护 index：读走 len 个字节
    void retrieve_all_index();                    // 维护 index：清空
    std::string retrieve_as_string(size_t len);              // 读走 len 个字节
    std::string retrieve_all_as_string();

public:
    explicit Buffer(size_t initSize = kInitialSize);
    ~Buffer();

    size_t readable_bytes_size() const;
    size_t writable_bytes_size() const;

    std::string read_from_buffer();
    void write_to_buffer(const char* data, size_t len);
    void write_to_buffer(const std::string& str);

    ssize_t read_from_fd(int fd, int* savedErrno); // fd ==> buffer: 从 fd 读数据写入 buffer 的 writable 区域（写入 buffer）
    ssize_t write_to_fd(int fd, int* savedErrno);  // buffer ==> fd: 把 buffer 的 readable 区域写入 fd（从 buffer 读出）
};
