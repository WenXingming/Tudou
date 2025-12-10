#include "EventLoopThreadPool.h"
#include "EventLoopThread.h"

# include "EventLoop.h"

EventLoopThreadPool::EventLoopThreadPool(size_t numThreads)
    :
    /* mainLoop(new EventLoop())
    , */ numThreads(numThreads)
    , ioLoopThreadPool(numThreads)
    , mtx()
    , ioLoops()
    , ioLoopsIndex(0) {

    // 向线程池提交任务，运行每个 IO 线程的事件循环
    for (int i = 0; i < static_cast<int>(numThreads); ++i) {
        ioLoopThreadPool.submit_task([this]() {
            std::unique_ptr<EventLoop> loop(new EventLoop());
            {
                // 保护共享资源 ioLoops
                std::unique_lock<std::mutex> lock(mtx);
                ioLoops.push_back(loop.get());
            }
            loop->loop();
            });
    }

}

EventLoop* EventLoopThreadPool::get_next_loop() {
    {
        std::unique_lock<std::mutex> lock(mtx);
        if (ioLoops.empty()) {
            return nullptr;
        }
        auto& ioLoop = ioLoops[ioLoopsIndex];
        ioLoopsIndex = (ioLoopsIndex + 1) % ioLoops.size();
        return ioLoop;
    }
}
