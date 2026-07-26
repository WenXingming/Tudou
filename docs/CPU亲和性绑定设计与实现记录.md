# CPU 亲和性绑定设计与实现记录

Tudou 在主从 Reactor 和 One-Loop-Per-Thread 模型上增加可选的 CPU 亲和性绑定，让主线程和 I/O 线程尽量稳定地运行在指定 CPU 上。

# Situation — 情境

多线程网络服务中，线程可能被操作系统调度到不同 CPU，带来额外的上下文切换和缓存失效。对于延迟敏感的 Reactor 线程，希望线程归属更稳定。

# Task — 任务

需要提供一个可选配置：

1. 主 Reactor 线程绑定 CPU 0；
2. I/O 线程按顺序绑定后续 CPU；
3. 不开启绑定时保持原有行为；
4. 绑定失败不能破坏线程池的基本启动和销毁流程。

# Action — 设计与实现

### 线程分配策略

```text
Main Reactor  → CPU 0
IO Loop 1     → CPU 1
IO Loop 2     → CPU 2
...
```

工作线程使用 `(i + 1) % numCores` 计算目标 CPU，避免线程数超过核心数时访问越界。

### EventLoopThread

构造函数接收 `cpuCore`，默认值 `-1` 表示不绑定。线程入口中使用 Linux 的 `pthread_setaffinity_np` 设置亲和性。

### EventLoopThreadPool

线程池接收 `pinCpu` 配置：

- 创建主 loop 时绑定当前线程到 CPU 0；
- 创建 I/O 线程时读取 `std::thread::hardware_concurrency()`；
- 按分配策略为每个工作线程设置目标 CPU。

### TcpServer

`TcpServer` 提供 `enable_cpu_affinity(bool)` 作为配置门面，将配置传递给 `EventLoopThreadPool`，业务层不需要直接接触线程实现。

# Result — 结果

- CPU 亲和性变成可选配置，不影响默认启动路径；
- 主线程和 I/O 线程可以按策略绑定到目标 CPU；
- 相关线程池测试能够验证开启绑定时正常启动和销毁；
- 亲和性只影响调度策略，不改变连接分配和事件循环逻辑。

# 验证记录

新增测试 `SetCpuAffinityDoesNotThrowOrError`，验证开启亲和性后线程池可以正常启动：

```cpp
TEST(EventLoopThreadPoolTest, SetCpuAffinityDoesNotThrowOrError) {
    EventLoopThreadPool pool(
        "affinity_test", 2,
        EventLoopThreadPool::ThreadInitCallback(), true);
    EXPECT_NO_THROW(pool.start());
}
```

测试日志显示主线程和两个 I/O 线程均完成绑定，相关测试通过。

完整测试中另有 `EpollPollerTest.ChannelRegisterAndUnregisterViaEventLoop` 历史失败。原因是当前代码采用惰性注册，`Channel` 在 `enable_reading()` 前不会登记到 Poller；该失败与 CPU 亲和性改动无关。

# 核心设计要点提炼

| 设计点 | 说明 |
| :--- | :--- |
| 可选绑定 | 默认不改变原有线程行为，通过配置开启。 |
| 线程分配 | 主线程使用 CPU 0，I/O 线程顺序分配后续核心。 |
| 配置边界 | `TcpServer` 提供门面，线程池负责具体实现。 |
| 失败隔离 | 亲和性配置不应影响线程池基本生命周期。 |

# 面试核心问答总结

## Q1：为什么要做 CPU 亲和性？

减少线程在不同 CPU 之间迁移带来的调度和缓存开销，改善延迟稳定性。但它是优化手段，不是功能正确性的前提。

## Q2：CPU 核心不够时怎么办？

使用取模策略循环分配，保证目标 CPU 编号落在可用范围内；实际绑定仍应由系统返回结果确认。

## Q3：为什么把配置放在 TcpServer？

`TcpServer` 是应用侧入口，负责把配置传递给线程池；这样业务不需要了解线程创建和 affinity 的底层细节。
