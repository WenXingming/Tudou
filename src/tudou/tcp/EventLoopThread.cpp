/**
 * @file EventLoopThread.cpp
 * @brief 将 EventLoop 和 线程绑定的封装类
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 */

#include "EventLoopThread.h"
#include "EventLoop.h"

EventLoopThread::EventLoopThread(const ThreadInitCallback& cb)
    : loop_(nullptr)
    , thread_(nullptr) // 默认初始化为空指针
    , mtx_()
    , condition_()
    , initCallback_(cb)
    , started_(false) {

}

EventLoopThread::~EventLoopThread() {
    // 如果想进一步把临界区缩小到只保护“取指针”，通常要配合更强的生命周期约束？
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (loop_) {
            loop_->quit();
        }
    }
    // 退出 loop 和线程（保持同步）
    if (thread_ && thread_->joinable()) {
        thread_->join();
    }
}

void EventLoopThread::start() {
    // 启动线程，线程内创建 EventLoop 对象并启动事件循环
    thread_.reset(new std::thread(&EventLoopThread::thread_func, this));

    // 线程创建是异步的。等待线程创建好 EventLoop 对象后再返回（确保调用 get_loop() 能够返回有效指针）
    {
        std::unique_lock<std::mutex> lock(mtx_);
        while (loop_ == nullptr) { // 防止虚假唤醒
            condition_.wait(lock); // 只能使用 unique_lock，因为 condition_variable 的 wait 需要释放锁
        }
    }

    started_ = true;
    return;
}

void EventLoopThread::thread_func() {
    // 在所属线程内创建 EventLoop 对象（构造函数内会记录所属线程 id 等。这是 one loop per thread 的关键）
    std::unique_ptr<EventLoop> eventLoop(new EventLoop());

    // 在 EventLoop 创建完成、进入 loop() 之前，调用初始化回调函数（如果传入了的话）
    if (initCallback_) {
        initCallback_(eventLoop.get());
    }
    // 将创建好的 EventLoop 对象指针传递给调用线程。通知调用线程可以返回 EventLoop 指针了
    {
        std::lock_guard<std::mutex> lock(mtx_);
        loop_ = std::move(eventLoop);
        condition_.notify_one();
    }
    // 启动事件循环
    loop_->loop();
    // 事件循环退出后，清理资源
    {
        std::lock_guard<std::mutex> lock(mtx_);
        loop_.reset();
    }
}
