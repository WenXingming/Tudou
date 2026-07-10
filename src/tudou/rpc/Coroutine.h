/**
 * @file Coroutine.h
 * @brief 基于 Boost.Coroutine2 的有栈协程上下文封装
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#pragma once

#include <boost/coroutine2/all.hpp>
#include <functional>
#include <memory>

class EventLoop;

namespace tudou {
namespace rpc {

class Coroutine : public std::enable_shared_from_this<Coroutine> {
public:
    using coro_t = boost::coroutines2::coroutine<void>;

    /**
     * @brief 构造函数，初始化并启动协程到第一个 yield 点
     * @param loop 协程所在的 EventLoop 线程指针
     * @param func 协程要运行的函数体
     */
    Coroutine(EventLoop* loop, std::function<void()> func);
    
    /**
     * @brief 析构函数
     */
    ~Coroutine();

    // 禁用拷贝构造和赋值
    Coroutine(const Coroutine&) = delete;
    Coroutine& operator=(const Coroutine&) = delete;

    /**
     * @brief 恢复运行该协程（从上一个 yield 挂起点继续向下）
     */
    void resume();

    /**
     * @brief 挂起当前协程运行，控制权返回给调用 resume() 的地方
     */
    void yield();

    /**
     * @brief 获取协程所绑定的 EventLoop 指针
     */
    EventLoop* get_loop() const { return loop_; }

public:
    // 线程局部变量：当前正在运行的协程实例指针
    static thread_local Coroutine* t_current_coroutine;

private:
    EventLoop* loop_;
    std::function<void()> func_;
    std::unique_ptr<coro_t::pull_type> pull_;
    coro_t::push_type* push_ = nullptr;
};

} // namespace rpc
} // namespace tudou
