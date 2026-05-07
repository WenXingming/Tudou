// ============================================================================
// Buffer.h
// Buffer 是 TCP 子系统的纯工具层，负责把 fd 读写和应用层字符串搬运统一成稳定的缓冲区契约。
//
// 成员函数调用树（[公有]/[私有] 标注接口层级）：
//
// Buffer.h
// └── Buffer
//     ├── Buffer(initialSize)                     # [公有] 构造：预留 prepend 区并初始化读写指针
//     ├── ~Buffer()                               # [公有] 析构：释放底层字节数组
//     ├── read_from_buffer(len)                   # [公有] 读走指定字节数并推进读指针
//     │   ├── readable_start_ptr() const          # [私有] 定位当前可读区首地址
//     │   └── maintain_read_index(len)            # [私有] 按实际消费量维护索引
//     │       └── maintain_all_index()            # [私有] 读空缓冲区时回到初始索引
//     ├── read_from_buffer()                      # [公有] 读走全部可读数据
//     │   └── read_from_buffer(readable_bytes())  # [公有] 复用定长读取路径
//     ├── write_to_buffer(data, len)              # [公有] 把原始内存追加到缓冲区
//     │   └── make_space(len)                     # [私有] 不够写时先挪动再按需扩容
//     │       └── prependable_bytes() const       # [私有] 计算头部可复用空间，决定是否搬移数据
//     ├── write_to_buffer(str)                    # [公有] 把字符串追加到缓冲区
//     │   └── write_to_buffer(str.data(), str.size())  # [公有] 复用原始内存写入路径
//     ├── read_from_fd(fd, &err)                  # [公有] 通过 readv 把 fd 数据搬入缓冲区
//     │   └── write_to_buffer(extraBuf, ...)      # [公有] 主缓冲放不下时把溢出数据继续写回 Buffer
//     ├── write_to_fd(fd, &err)                   # [公有] 把可读区数据写入 fd 并推进读指针
//     │   └── maintain_read_index(n)              # [私有] 写成功后消费对应字节数
//     │       └── maintain_all_index()            # [私有] 缓冲区写空时整体复位索引
//     ├── readable_bytes() const                  # [公有] 返回当前可读字节数
//     └── writable_bytes() const                  # [公有] 返回当前可写字节数
// ============================================================================

#pragma once

#include <sys/types.h>

#include <vector>
#include <string>

// Buffer 只负责字节搬运与空间管理，不参与任何业务编排。
class Buffer {
public:
    explicit Buffer(size_t initialSize = kInitialSize_);
    ~Buffer();

    std::string read_from_buffer(size_t len); // 读取指定字节并推进读指针。
    std::string read_from_buffer();
    void write_to_buffer(const char* data, size_t len); // 顺序追加原始字节。
    void write_to_buffer(const std::string& str);

    ssize_t read_from_fd(int fd, int* savedErrno); // 通过 readv 把 fd 数据追加到缓冲区。
    ssize_t write_to_fd(int fd, int* savedErrno); // 把当前可读数据刷入 fd。

    size_t readable_bytes() const;
    size_t writable_bytes() const;

private:
    size_t prependable_bytes() const;
    const char* readable_start_ptr() const;
    void maintain_read_index(size_t len);
    void maintain_all_index();
    void make_space(size_t len); // 优先复用 prepend 区，不够再扩容。

private:
    static const size_t kCheapPrepend_;
    static const size_t kInitialSize_;

    std::vector<char> buffer_; // 底层连续字节数组，统一承载 prepend/readable/writable 三个区域。
    size_t readIndex_; // 当前可读区域起点。
    size_t writeIndex_; // 当前可读区域终点，也是可写区域起点。
};
