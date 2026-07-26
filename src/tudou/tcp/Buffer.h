// ============================================================================
// Buffer 管理连续字节数组，为 TCP 连接提供统一的内存和 fd 读写接口。
// fd 由调用方负责，Buffer 只维护数据内容和读写索引。
//
// 内存布局：
// +-------------------+------------------+------------------+
// |   prependable     |     readable     |     writable     |
// +-------------------+------------------+------------------+
// 0              readIndex_          writeIndex_          size
// ============================================================================

#pragma once

#include <sys/types.h>

#include <string>
#include <vector>

class Buffer {
public:
    explicit Buffer(size_t initialSize = kInitialSize);
    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    Buffer(Buffer&&) = default;
    Buffer& operator=(Buffer&&) = default;

    // fd → Buffer → 应用层
    ssize_t read_from_fd(int fd, int& savedErrno);      // 通过 readv 把 fd 数据追加到缓冲区，失败时写入 errno。
    std::string read_from_buffer(size_t len);           // 读取指定字节并推进读指针。
    std::string read_from_buffer();                     // 读取全部可读数据并推进读指针。
    void advance_read_index(size_t len);                // 仅推进读指针；len 不得超过当前可读字节数。

    // 应用层 → Buffer → fd
    void write_to_buffer(const char* data, size_t len); // 顺序追加原始字节。
    void write_to_buffer(const std::string& str);       // 追加字符串内容。
    ssize_t write_to_fd(int fd, int& savedErrno);       // 把当前可读数据刷入 fd，失败时写入 errno。

    // 状态查询
    size_t readable_bytes() const;
    size_t writable_bytes() const;
    const char* readable_start_ptr() const;             // 返回可读区域首地址，不推进读指针。

private:
    void maintain_read_index(size_t len);
    void maintain_all_index();

    void make_space(size_t len); // 优先复用 prepend 区，不够再扩容。

    size_t prependable_bytes() const;

private:
    static const size_t kCheapPrepend;
    static const size_t kInitialSize;
    static const size_t kStackBufSize;

    std::vector<char> buffer_;          // 底层连续字节数组，统一承载 prepend/readable/writable 三个区域。
    size_t readIndex_;                  // 当前可读区域起点。
    size_t writeIndex_;                 // 当前可读区域终点，也是可写区域起点。
};
