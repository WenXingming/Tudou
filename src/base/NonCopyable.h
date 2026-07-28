// ============================================================================
// 不可拷贝也不可移动的基类
// ============================================================================

#pragma once

// NonCopyable 作为受保护继承的契约基类，专门负责删除复制与移动语义。
class NonCopyable {
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

    NonCopyable(NonCopyable&&) = delete;
    NonCopyable& operator=(NonCopyable&&) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};