/**
 * @file NonCopyable.h
 * @brief NonCopyable 被继承以后，派生类对象无法进行拷贝构造和赋值（派生类对象可以正常的构造和析构）
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/Multi_IO
 */

#pragma once

class NonCopyable {
public:
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;

protected: // 不影响继承派生
    NonCopyable() {}
    ~NonCopyable() {}
};