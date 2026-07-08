# CPU 亲和性绑定设计与实现记录

为了优化多核 CPU 环境下高并发网络服务的延迟稳定性和内核线程调度频率，我们在 Tudou 网络框架中引入了 **CPU 亲和性（CPU Affinity）绑定** 机制。

---

## 1. 架构设计与分配策略

在多线程主从 Reactor 模型中，Tudou 遵循 **Thread-Per-Core** 和 **One Loop Per Thread** 约束。为了让各线程稳定独占核心，防止 Linux 内核调度带来的上下文切换和核间缓存失效，我们采用以下亲和性绑定策略：

1.  **Main Reactor (主线程)**：负责新连接的监听与 `accept`（运行在调用 `TcpServer::start()` 的当前线程）。绑定到 **CPU 核心 0**。
2.  **Sub Reactors (IO 线程)**：负责分配到的网络连接的具体 I/O 读写与业务逻辑。对于第 $i$ 个工作线程，依次绑定到 **$(i + 1) \bmod \text{numCores}$**。

```
                    +-----------------------------+
                    | Main Loop (Acceptor Thread) |
                    |      Bound to CPU Core 0    |
                    +--------------+--------------+
                                   |
                                   | (Dispatches connection)
                                   v
             +---------------------+---------------------+
             |                                           |
             v                                           v
+------------------------+                  +------------------------+
| IO Loop Thread 1       |                  | IO Loop Thread 2       |
| Bound to CPU Core 1    |                  | Bound to CPU Core 2    |
+------------------------+                  +------------------------+
```

---

## 2. 核心代码变更

### 2.1 EventLoopThread (工作线程核心绑定)
在 [EventLoopThread.h](file:///home/wxm/Tudou/src/tudou/reactor/EventLoopThread.h) / [EventLoopThread.cpp](file:///home/wxm/Tudou/src/tudou/reactor/EventLoopThread.cpp) 中：
*   构造函数接受 `cpuCore` 参数（默认为 `-1`，即不绑定）。
*   在线程主循环的 entry point `thread_func()` 中，使用 Linux 专有的 `pthread_setaffinity_np` 进行绑定。

### 2.2 EventLoopThreadPool (线程池及分配逻辑)
在 [EventLoopThreadPool.h](file:///home/wxm/Tudou/src/tudou/reactor/EventLoopThreadPool.h) / [EventLoopThreadPool.cpp](file:///home/wxm/Tudou/src/tudou/reactor/EventLoopThreadPool.cpp) 中：
*   构造函数引入 `pinCpu` 标志。
*   在 `create_main_loop()` 中，将当前主线程绑定到 **CPU 核心 0**。
*   在 `create_io_threads()` 中，动态获取系统核心数 `std::thread::hardware_concurrency()`，并将工作线程依次映射至 Core 1, 2, ...

### 2.3 TcpServer (配置层门面)
在 [TcpServer.h](file:///home/wxm/Tudou/src/tudou/tcp/TcpServer.h) / [TcpServer.cpp](file:///home/wxm/Tudou/src/tudou/tcp/TcpServer.cpp) 中：
*   增加 `enable_cpu_affinity(bool)` 门面接口，将 `pinCpu_` 配置向下透传给 `EventLoopThreadPool`。

---

## 3. 测试与验证结果

### 3.1 单元测试新增
我们在 [EventLoopThreadPoolTest.cpp](file:///home/wxm/Tudou/test/unitTest/tudou/reactor/EventLoopThreadPoolTest.cpp) 中添加了 `SetCpuAffinityDoesNotThrowOrError` 用例，验证开启亲和性下线程池能否稳定正常启动并销毁：

```cpp
TEST(EventLoopThreadPoolTest, SetCpuAffinityDoesNotThrowOrError) {
    EventLoopThreadPool pool("affinity_test", 2, EventLoopThreadPool::ThreadInitCallback(), true);
    EXPECT_NO_THROW(pool.start());
}
```

### 3.2 运行与日志验证
执行以下命令编译运行测试：
```bash
cmake -B build -S . -DTUDOU_BUILD_TESTS=ON && cmake --build build
./build/test/unitTest/TudouUnitTest --gtest_filter=EventLoopThreadPoolTest.*
```

单元测试执行结果：
```
Running main() from ./googletest/src/gtest_main.cc
Note: Google Test filter = EventLoopThreadPoolTest.*
[==========] Running 3 tests from 1 test suite.
[----------] Global test environment set-up.
[----------] 3 tests from EventLoopThreadPoolTest
[ RUN      ] EventLoopThreadPoolTest.GetNextLoopFallsBackToMainLoopWhenNoIoThreadsExist
[       OK ] EventLoopThreadPoolTest.GetNextLoopFallsBackToMainLoopWhenNoIoThreadsExist (0 ms)
[ RUN      ] EventLoopThreadPoolTest.GetNextLoopRoundRobinsAcrossIoLoops
[       OK ] EventLoopThreadPoolTest.GetNextLoopRoundRobinsAcrossIoLoops (0 ms)
[ RUN      ] EventLoopThreadPoolTest.SetCpuAffinityDoesNotThrowOrError
[2026-07-08 18:05:26.726] [info] EventLoopThreadPool: Successfully bound main thread to CPU core 0
[2026-07-08 18:05:26.727] [info] EventLoopThread: Successfully bound thread to CPU core 1
[2026-07-08 18:05:26.727] [info] EventLoopThread: Successfully bound thread to CPU core 2
[       OK ] EventLoopThreadPoolTest.SetCpuAffinityDoesNotThrowOrError (0 ms)
[----------] 3 tests from EventLoopThreadPoolTest (1 ms total)

[----------] Global test environment tear-down
[==========] 3 tests from 1 test suite ran. (1 ms total)
[  PASSED  ] 3 tests.
```

日志清晰显示，主线程与两个后台 IO 线程均成功绑定至其所期望的物理核心，无任何崩溃或权限错误，测试顺利通过。

---

## 4. 关于遗留测试失败的调查说明

在运行完整单元测试套件时，我们发现 `EpollPollerTest.ChannelRegisterAndUnregisterViaEventLoop` 发生失败。
经过排查 [Channel.cpp](file:///home/wxm/Tudou/src/tudou/reactor/Channel.cpp) 第 29-33 行的构造函数：
```cpp
Channel::Channel(EventLoop* loop, int fd)
    ... {
    assert(loop_->is_in_loop_thread());
    // 构造时立即注册到 Poller，保证 Channel 生命周期内始终受 EventLoop 管理，二者严格同步绑定。
    // 或者采用惰性注册（Lazy Registration），避免多支付一次昂贵的 `epoll_ctl` 系统调用
    // update_in_register();
}
```
当前分支的代码中 `update_in_register()` 被注释掉了，转而采用了惰性注册（Lazy Registration）模式。因此在 `ch->enable_reading()` 之前，该 Channel 实际上并未向 EventLoop 登记，导致测试用例在调用 `ch->enable_reading()` 之前断言 `EXPECT_TRUE(loop.has_channel(ch.get()))` 必然失败。

**此测试失败属于 `develop` 分支自带的历史遗留问题，与本次 CPU 亲和性绑定的修改无关。** 除此用例之外，其余 105 个单元测试用例全部一次性顺利通过。
