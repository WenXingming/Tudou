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
    // ioLoops(),
    ioLoopsIndex(0),
    name(nameArg),
    started(false),
    numThreads(numThreadsArg) {

    mainLoop->assert_in_loop_thread();
}

void EventLoopThreadPool::start(const ThreadInitCallback& cb) {
    assert(!started);


    started = true;
    for (int i = 0; i < numThreads; ++i) {
        std::unique_ptr<EventLoopThread> ioThread(new EventLoopThread(cb));
        ioLoopThreads.push_back(std::move(ioThread));
        ioLoopThreads.back()->start_loop();
    }

    if (numThreads == 0 && cb) {
        cb(mainLoop.get());
    }
}

EventLoop* EventLoopThreadPool::get_next_loop() {
    assert(started);
    mainLoop->assert_in_loop_thread();

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
    std::vector<EventLoop*> loops;
    for (int i = 0; i < ioLoopThreads.size(); ++i) {
        loops.push_back(ioLoopThreads[i]->get_loop());
    }
    return std::move(loops);
}


// #include "EventLoopThreadPool.h"
// #include "EventLoopThread.h"

// # include "EventLoop.h"

// EventLoopThreadPool::EventLoopThreadPool(size_t numThreads)
//     :
//     /* mainLoop(new EventLoop())
//     , */ numThreads(numThreads)
//     , ioLoopThreadPool(numThreads)
//     , mtx()
//     , ioLoops()
//     , ioLoopsIndex(0) {

//     // 向线程池提交任务，运行每个 IO 线程的事件循环
//     for (int i = 0; i < static_cast<int>(numThreads); ++i) {
//         ioLoopThreadPool.submit_task([this]() {
//             std::unique_ptr<EventLoop> loop(new EventLoop());
//             {
//                 // 保护共享资源 ioLoops
//                 std::unique_lock<std::mutex> lock(mtx);
//                 ioLoops.push_back(loop.get());
//             }
//             loop->loop();
//             });
//     }

// }

// EventLoop* EventLoopThreadPool::get_next_loop() {
//     {
//         std::unique_lock<std::mutex> lock(mtx);
//         if (ioLoops.empty()) {
//             return nullptr;
//         }
//         auto& ioLoop = ioLoops[ioLoopsIndex];
//         ioLoopsIndex = (ioLoopsIndex + 1) % ioLoops.size();
//         return ioLoop;
//     }
// }