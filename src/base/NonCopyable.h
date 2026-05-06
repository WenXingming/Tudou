// ============================================== //
// NonCopyable.h
// 不可拷贝也不可移动的 mixin 基类，用于把所有权语义固定在类型边界。
// ============================================== //

#pragma once

// NonCopyable 作为受保护继承的契约基类，专门负责删除复制与移动语义。
class NonCopyable {
public:
    NonCopyable(const NonCopyable&) = delete;

    /**
     * @brief 禁止拷贝赋值。
     * @param other 试图被赋值的源对象。
     * @return NonCopyable& 该操作被删除，不可用。
     */
    NonCopyable& operator=(const NonCopyable&) = delete;

    NonCopyable(NonCopyable&&) = delete;

    /**
     * @brief 禁止移动赋值。
     * @param other 试图被移动赋值的源对象。
     * @return NonCopyable& 该操作被删除，不可用。
     */
    NonCopyable& operator=(NonCopyable&&) = delete;

protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
};