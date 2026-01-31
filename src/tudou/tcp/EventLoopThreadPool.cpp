/**
 * @file EventLoopThreadPool.cpp
 * @brief 将多个 EventLoopThread 组合成线程池的封装类
 * @author wenxingming
 * @project: https://github.com/WenXingming/Tudou
 * @details
*/

#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"
#include "EventLoop.h"

#include <cassert>

EventLoopThreadPool::EventLoopThreadPool(const std::string& nameArg, int numThreadsArg, const ThreadInitCallback& cb) :
    mainLoop(new EventLoop()),
    ioLoopThreads(),
    ioLoopsIndex(0),
    name(nameArg),
    numThreads(numThreadsArg),
    initCallback(cb),
    started(false) {

    mainLoop->assert_in_loop_thread();
}

void EventLoopThreadPool::start() {
    mainLoop->assert_in_loop_thread();
    assert(!started);

    for (int i = 0; i < numThreads; ++i) {
        std::unique_ptr<EventLoopThread> ioThread(new EventLoopThread(initCallback));
        ioThread->start();
        ioLoopThreads.push_back(std::move(ioThread));
    }

    if (numThreads == 0 && initCallback) {
        initCallback(mainLoop.get());
    }

    started = true;
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    mainLoop->assert_in_loop_thread();
    assert(started);

    // 默认轮询算法获取下一个 IO 线程的 EventLoop 指针
    EventLoop* loop = mainLoop.get();
    if (!ioLoopThreads.empty()) {
        loop = ioLoopThreads[ioLoopsIndex]->get_loop();
        // ++ioLoopsIndex;
        // if (ioLoopsIndex >= ioLoopThreads.size()) {
        //     ioLoopsIndex = 0;
        // }
        ioLoopsIndex = (ioLoopsIndex + 1) % ioLoopThreads.size(); // 映射到 [0, size)
    }
    return loop;
}

std::vector<EventLoop*> EventLoopThreadPool::get_all_loops() const {
    assert(started);
    std::vector<EventLoop*> loops;
    for (int i = 0; i < ioLoopThreads.size(); ++i) {
        loops.push_back(ioLoopThreads[i]->get_loop());
    }
    return std::move(loops);
}
