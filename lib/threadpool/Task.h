/**
 * @file Task.h
 * @brief Declaration of class: Task（Abstract task）
 * @author wenxingming
 * @note My project address: https://github.com/WenXingming/ThreadPool
 */

#pragma once
#include <iostream>
#include <functional>
#include <chrono>
#include <climits>

namespace wxm {

/// =======================================================================
/// NOTE: Declaration of class: Task（Abstract task）
class Task {
private:
    int priority;										// 优先级，数字越大优先级越高
    std::chrono::steady_clock::time_point timestamp;	// 时间戳（不用整数，精度不够），用于 FCFS
    std::function<void()> function;						// 实际的任务函数

public:
    Task() // 安全：在类定义内部实现的成员函数默认是内联的（多个源文件同时包含 task 类，链接时不会重定义）
        : priority(INT_MIN)
        , timestamp(std::chrono::steady_clock::now())
        , function(nullptr) {
    }
    Task(std::function<void()> _func, int _priority = 0) // 任务函数是通用 function. 只有线程池 submit_task() 入口是模板参数列表
        : priority(_priority)
        , timestamp(std::chrono::steady_clock::now())
        , function(_func) {
    }
    ~Task() {}

    // 注意比较 (Compare) 形参的定义，使得若其第一参数在弱序中先于其第二参数则返回 true
    // 1. 优先级高的在前（优先级调度）。若返回 true，表示当前对象在弱序中先于 other；默认最大堆，所以当前对象在堆底，other 在堆顶
    // 2. 时间戳小的在前（FCFS 调度）。若返回 true，表示当前对象在弱序中先于 other；默认最大堆，第一参数（当前对象）位于堆底
    bool operator<(const Task& other) const {
        return priority < other.priority ? true :
            priority > other.priority ? false :
            !(timestamp < other.timestamp); // timestamp >= other.timestamp
    }

    int get_priority() const { return priority; }

    //将 time_point 转换为自纪元以来的秒（seconds）计数
    int64_t get_timestamp() const {
        auto duration = timestamp.time_since_epoch();
        return std::chrono::duration_cast<std::chrono::seconds>(duration).count();
    }

    void execute() {
        if (function) {
            function();
        }
        else throw std::runtime_error("task's function is empty, can't not execute.");
        /// TODO: 注释掉 std::cout
        std::cout << "priority is: " << priority
            << ". time is: " << timestamp.time_since_epoch().count() << std::endl; // debug, 测试优先级调度和 FCFS。实际使用时注释掉
    }
};


}