// ============================================================================
// Buffer.h
// Buffer 是 TCP 子系统的纯工具层，负责把 fd 读写和应用层字符串搬运统一成稳定的缓冲区契约。
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

    /**
     * @brief 从缓冲区中读走指定长度的数据。
     * @param len 期望读取的字节数；超过可读长度时会安全截断。
     * @return 读取出的字节串。
     */
    std::string read_from_buffer(size_t len);

    /**
     * @brief 读走当前缓冲区中的全部可读数据。
     * @return 当前全部可读数据。
     */
    std::string read_from_buffer();

    /**
     * @brief 把一段原始内存写入缓冲区。
     * @param data 数据起始地址。
     * @param len 数据长度。
     */
    void write_to_buffer(const char* data, size_t len);

    /**
     * @brief 把字符串写入缓冲区。
     * @param str 待写入的字符串。
     */
    void write_to_buffer(const std::string& str);

    /**
     * @brief 从 fd 读取数据并追加到缓冲区。
     * @param fd 数据来源 fd。
     * @param savedErrno 输出参数，返回系统错误码。
     * @return 本次读取的字节数；负值表示读取失败。
     */
    ssize_t read_from_fd(int fd, int* savedErrno);

    /**
     * @brief 把缓冲区可读数据写入 fd。
     * @param fd 数据目标 fd。
     * @param savedErrno 输出参数，返回系统错误码。
     * @return 本次写出的字节数；负值表示写入失败。
     */
    ssize_t write_to_fd(int fd, int* savedErrno);

    /**
     * @brief 获取可读字节数。
     * @return 当前可读字节数。
     */
    size_t readable_bytes() const;

    /**
     * @brief 获取可写字节数。
     * @return 当前可写字节数。
     */
    size_t writable_bytes() const;

private:
    /**
     * @brief 获取预留头部空间的字节数。
     * @return 当前 prepend 区域字节数。
     */
    size_t prependable_bytes() const;

    /**
     * @brief 获取当前可读区域的起始指针。
     * @return 可读区域首地址。
     */
    const char* readable_start_ptr() const;

    /**
     * @brief 消费指定数量的可读字节并维护索引。
     * @param len 已消费字节数。
     */
    void maintain_read_index(size_t len);

    /**
     * @brief 重置读写索引到初始位置。
     */
    void maintain_all_index();

    /**
     * @brief 确保缓冲区至少还能写入指定字节数。
     * @param len 需要补齐的可写空间。
     */
    void make_space(size_t len);

private:
    static const size_t kCheapPrepend_;
    static const size_t kInitialSize_;

    std::vector<char> buffer_; // 底层连续字节数组，统一承载 prepend/readable/writable 三个区域。
    size_t readIndex_; // 当前可读区域起点。
    size_t writeIndex_; // 当前可读区域终点，也是可写区域起点。
};
