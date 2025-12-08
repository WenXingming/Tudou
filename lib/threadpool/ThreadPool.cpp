/**
 * @file ThreadPool.cpp
 * @brief 一个通用、高效的 C++ 线程池实现。
 * @details 该线程池支持提交任何可调用对象作为任务，并能异步获取任务的返回值。利用了 C++11 的 std::packaged_task 和 std::future 实现任务管理和结果获取。
 * @author wenxingming
 * @date 2025-08-26
 * @note My project address: https://github.com/WenXingming/ThreadPool
 * @cite: https://github.com/progschj/ThreadPool/tree/master
 */

#include "ThreadPool.h"
namespace wxm {

/// @brief 默认构造函数创建硬件核数的线程数量
ThreadPool::ThreadPool()
    : ThreadPool(std::thread::hardware_concurrency() == 0 ? 2 : std::thread::hardware_concurrency()) {}


ThreadPool::ThreadPool(int _threadsSize, int _maxTasksSize, bool _openAutoExpandReduce, int _maxWaitTime)
    : stopFlag(false)
    , maxTasksSize(_maxTasksSize), openAutoExpandReduce(_openAutoExpandReduce), maxWaitTime(_maxWaitTime) {

    int hardwareSize = std::thread::hardware_concurrency() == 0 ? 2 : std::thread::hardware_concurrency();
    int size = std::min(_threadsSize, 2 * hardwareSize);
    for (int i = 0; i < size; ++i) {
        auto threadFunc = [this]() {
            this->process_task();
            };
        std::thread t(threadFunc);
        threads.push_back(std::move(t)); // thread　对象只支持　move，一个线程最多只能由一个 thread 对象持有
    }
    std::cout << "thread pool is created success, size is: " << threads.size() << std::endl;
}


ThreadPool::~ThreadPool() {
    stopFlag = true;
    notEmpty.notify_all();
    for (auto& thread : threads) { 	// 一个线程只能被一个 std::thread 对象管理, 复制构造/赋值被禁用。所以用 &
        thread.join();
    }
    std::cout << "current thread pool size: " << threads.size() << ", all threads joined." << std::endl;
    std::cout << "thread pool is destructed success, and tasks are all finished." << std::endl;
}

int ThreadPool::get_thread_pool_size() {
    std::unique_lock<std::mutex> uniqueLock(threadsMutex);
    return threads.size();
}


void ThreadPool::process_task() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> uniqueLock(tasksMutex);
            bool retWait = notEmpty.wait_for(uniqueLock, std::chrono::milliseconds(maxWaitTime), [this]() {
                return (!tasks.empty() || stopFlag);
                });

            if (retWait) {
                if (!tasks.empty()) {						// 要先把任务处理完
                    task = std::move(tasks.top());		// std::packaged_task<> 只支持 move，禁止拷贝
                    tasks.pop();
                }
                else return; // 线程池终止。退出循环（线程），后续 join()
            }
            else {
                uniqueLock.unlock();
                if (openAutoExpandReduce) reduce_thread_pool(std::this_thread::get_id()); // 超时，线程池处理速度高于任务提交速度，需要缩减线程池。被认为是耗时操作，手动解锁
                break; // 退出死循环，等待后续 join() 或 detach() 由操作系统回收资源
            }

        }
        if (task.get_priority() != INT_MIN) { // 取出了任务
            task.execute();
            notFull.notify_one();
        }
    }
}


void ThreadPool::expand_thread_pool() {
    std::unique_lock<std::mutex> uniqueLock(threadsMutex);
    int hardwareSize = std::thread::hardware_concurrency() == 0 ? 2 : std::thread::hardware_concurrency();
    if (threads.size() >= 2 * hardwareSize) { // 线程池最大线程数量设定为 2 * hardwareSize（如需要自定义设置再修改类，增添变量 maxThreadsSize）
        std::cout << "thread_pool is MAX_SIZE（2 * hardwareSize）: " << threads.size() << ", can't be expanded."
            << " you'd better slow down the speed of submitting task.\n";
        return;
    }
    else {
        auto threadFunc = [this]() {
            this->process_task();
            };
        std::thread t(threadFunc);
        threads.push_back(std::move(t));
        std::cout << "thread_pool auto expand successful, now size is: " << threads.size() << std::endl;
    }
}


void ThreadPool::reduce_thread_pool(std::thread::id threadId) {
    std::unique_lock<std::mutex> uniqueLock(threadsMutex);
    if (threads.size() <= 1) {
        std::cout << "thread_pool is 1: " << threads.size() << ", can't be reduced."
            << " please submit_task.\n";
        return;
    }
    else {
        int index = 0;
        for (int i = 0; i < threads.size(); ++i) {
            if (threads[i].get_id() == threadId) {
                index = i;
                break;
            }
        }
        std::thread t = std::move(threads[index]);
        threads.erase(threads.begin() + index);
        t.detach(); // 需要被销毁的线程本身调用 reduce_thread_pool，它并不能 join() 自己。所以让操作系统回收线程
        std::cout << "thread_pool auto reduce successful, now size is: " << threads.size() << std::endl;
    }
}

}