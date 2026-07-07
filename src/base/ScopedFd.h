// ============================================================================
// ScopedFd.h
// 拥有文件描述符（fd）所有权的 RAII 包装类。
// ============================================================================

#pragma once

#include "base/NonCopyable.h"
#include <unistd.h>

class ScopedFd : public NonCopyable {
public:
    ScopedFd() noexcept : fd_(-1) {}
    explicit ScopedFd(int fd) noexcept : fd_(fd) {}
    
    ~ScopedFd() {
        reset();
    }

    // 支持移动语义
    ScopedFd(ScopedFd&& other) noexcept : fd_(other.release()) {}
    
    ScopedFd& operator=(ScopedFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    int fd() const noexcept { return fd_; }
    bool valid() const noexcept { return fd_ >= 0; }

    int release() noexcept {
        int temp = fd_;
        fd_ = -1;
        return temp;
    }

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

private:
    int fd_;
};
