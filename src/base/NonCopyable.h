// ============================================================================
// NonCopyable.h
// 不可拷贝也不可移动的 mixin 基类，用于把所有权语义固定在类型边界。
//
// 成员函数调用树（[公有]/[受保护] 标注接口层级）：
//
// NonCopyable.h
// └── NonCopyable
//     ├── NonCopyable(copy)                      # [公有] 删除拷贝构造
//     │   ├── NonCopyable()                      # [受保护] 派生类继承链使用的默认构造
//     │   └── ~NonCopyable()                     # [受保护] 派生类继承链使用的默认析构
//     ├── operator=(copy)                        # [公有] 删除拷贝赋值
//     ├── NonCopyable(move)                      # [公有] 删除移动构造
//     └── operator=(move)                        # [公有] 删除移动赋值
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