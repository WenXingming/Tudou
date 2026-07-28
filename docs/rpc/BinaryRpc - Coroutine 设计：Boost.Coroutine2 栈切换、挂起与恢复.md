# BinaryRpc - Coroutine 设计：Boost.Coroutine2 栈切换、挂起与恢复

`binary::Coroutine` 是 Tudou 对 Boost.Coroutine2 有栈协程的最小封装。它负责创建协程执行上下文、在调用方与协程之间切换，并记录当前线程正在执行的协程；网络收发、请求关联和恢复时机由 `CoroutineChannel` 与 `EventLoop` 管理。

需要先明确它的边界：Coroutine 只解决“等待期间如何保留并恢复调用现场”，不是线程池、网络调度器或服务发现组件。

# Situation — 情境

Reactor 要求 EventLoop 线程不能阻塞。若协程客户端在发出 RPC 后直接等待：

```cpp
send_request();
future.get();
```

EventLoop 将停在 `future.get()`，无法继续处理 socket 可读事件，响应也就永远无法到达当前调用。

纯回调虽然不会阻塞，但会把原本连续的业务流程拆散：

```cpp
send_request([&](const Response& response) {
    continue_business(response);
});
```

有栈协程希望同时得到两点：

- 等待网络时把执行权交还 EventLoop；
- 业务代码仍按“调用 RPC → 得到响应 → 继续执行”的同步顺序书写。

# Task — 任务

Coroutine 需要提供一个足够小的执行原语：

- `resume()` 从 EventLoop/调用方进入协程；
- `yield()` 从协程返回 EventLoop/调用方；
- 挂起时保留完整调用栈、局部变量和执行位置；
- 恢复时从最近一次 `yield()` 后继续；
- `current()` 找到当前正在运行的协程；
- 异常和嵌套恢复后正确还原此前的当前协程；
- 只记录所属 EventLoop，不承担调度和网络职责。

# Action — Boost.Coroutine2 原理

## 1. 有栈协程保存完整函数调用链

普通函数返回后，其栈帧会被销毁。有栈协程挂起时不会销毁自己的栈，因此可以保留：

- 函数调用层级；
- 局部变量；
- 栈指针；
- 下一条要执行的指令位置；
- 上下文切换所需的寄存器状态。

例如 RPC 调用挂起时，协程栈可能是：

```text
业务函数
└── Protobuf Stub::Echo()
    └── CoroutineChannel::CallMethod()
        └── Coroutine::yield()
```

响应到达后恢复这块栈，`yield()` 返回，随后 `CallMethod()`、`Stub::Echo()` 依次返回，业务函数便能继续使用栈上的 response。

协程不是线程。它不会并行执行，也不会被操作系统抢占；`yield()` 和 `resume()` 都是程序主动发起的用户态上下文切换。

## 2. Boost.Context 完成底层上下文切换

Boost.Coroutine2 建立在 Boost.Context 之上。概念上，一次切换包含：

```text
保存当前执行现场
  → 当前栈指针和必要寄存器写入上下文记录
恢复目标执行现场
  → 加载目标栈指针和寄存器
跳到目标上次保存的指令位置继续执行
```

因此“下一次从哪里继续”不是 Tudou 用行号或函数指针手工记录的，而是 Boost.Context 在切换时保存和恢复的执行上下文。

## 3. pull_type 与 push_type 是同一条协程的两个端点

Tudou 使用：

```cpp
using Context = boost::coroutines2::coroutine<void>;
```

`void` 表示切换时不传递值。这个类型提供两个配对端点：

```text
Context::pull_type：调用方持有，用来进入或恢复协程
Context::push_type：协程函数获得，用来挂起并返回调用方
```

方向可以记为：

```text
调用方 ── pull/resume ──> 协程
调用方 <── push/yield ─── 协程
```

二者不是两个独立协程，也不是两份独立的用户任务。`pull_type` 拥有协程执行上下文和栈；`push_type` 是协程函数内部切回调用方的端点。

## 4. pull 与 push 的“返回”互相配对

这两个调用不是普通函数调用。它们会切换执行栈：

```text
调用方调用 pull()
  → 进入协程
  → 协程调用 push()
  → 调用方的 pull() 返回
```

以后调用方再次调用 pull()：

```text
调用方再次调用 pull()
  → 恢复协程
  → 上一次 push() 返回
  → 协程从 push() 后继续
```

可以压缩成一句话：

> `push()` 让当前 `pull()` 返回；下一次 `pull()` 又让上一次 `push()` 返回。

## 5. 为什么 push_ 只保存一次

Boost 创建协程时，把一个 `push_type&` 传入协程入口函数：

```cpp
[this](Context::push_type& yield) {
    push_ = &yield;
}
```

这个 `push_type` 对象代表“本协程返回调用方”的固定端点。协程入口函数从开始执行到最终返回期间始终存在，挂起只暂停函数执行，不会结束这次函数调用，因此该引用在协程生命周期内保持有效。

`push_` 保存的不是某一行代码、某次 `yield()` 的调用点或指令地址。它只是指向这个稳定的端点对象：

```text
push_ ──> Boost push_type 端点对象
                  │
                  └── 内部上下文记录随每次切换更新
```

当调用点从第一个 `yield()` 变为第二个 `yield()` 时：

- `push_type` 对象地址不变；
- Boost 内部保存的栈指针和下一条指令位置发生变化；
- 下一次 `resume()` 会恢复最新保存的执行现场。

所以 `push_` 只需在协程入口处保存一次。它是非拥有指针，生命周期依附于 `pull_` 所拥有的协程上下文。协程函数结束后不能再调用 `yield()`。

# Action — Tudou Coroutine 实现

## 6. 成员分别表达什么

```cpp
static thread_local Coroutine* currentCoroutine_;

EventLoop* loop_;
std::function<void()> function_;
std::unique_ptr<Context::pull_type> pull_;
Context::push_type* push_;
```


| 成员                | 作用                                   | 所有权                 |
| :-------------------- | :--------------------------------------- | :----------------------- |
| `currentCoroutine_` | 当前线程正在执行哪个 Coroutine         | 每线程独立的非拥有指针 |
| `loop_`             | 记录协程所属 EventLoop                 | 非拥有指针             |
| `function_`         | 用户任务函数                           | Coroutine 拥有         |
| `pull_`             | 调用方进入协程的端点，并拥有协程上下文 | `unique_ptr` 独占      |
| `push_`             | 协程返回调用方的端点                   | 借用 Boost 提供的对象  |

`Coroutine` 继承 `enable_shared_from_this`，是因为 RPC 响应回调需要持有协程，保证挂起期间协程对象和它拥有的栈不会被销毁。

## 7. 构造时先执行一次初始挂起

Boost 在构造 `pull_type` 时会立即进入协程入口函数。如果直接执行 `function_()`，任务就会在 `Coroutine` 构造期间运行，而且 `make_shared` 尚未完成，无法安全使用 `shared_from_this()`。

当前实现先保存 push 端点，再主动切回构造方：

```cpp
// 最原始的 Boost 协程创建
// using Context = boost::coroutines2::coroutine<void>;

// Context::pull_type coroutine([](Context::push_type& yield) {
//     std::cout << "A\n";

//     yield();

//     std::cout << "B\n";
// });

pull_ = std::make_unique<Context::pull_type>(
    [this](Context::push_type& yield) {
        push_ = &yield;
        (*push_)();
        function_();
    });
```

真实时序为：

```text
进入 Coroutine 构造函数
  → 创建 pull_type 和协程栈
  → Boost 立即进入协程入口 lambda
  → 保存 push_ 地址
  → 调用 push_，初始挂起
  → 返回 pull_type 构造过程
  → Coroutine 构造完成
  → 外部 shared_ptr 建立
```

此时执行位置停在：

```cpp
(*push_)(); // 正在初始挂起
function_(); // 尚未执行
```

第一次 `resume()` 才会让这次 `push_()` 返回并开始执行 `function_()`。

## 8. resume() 是进入协程的唯一入口

```cpp
void Coroutine::resume() {
    assert(pull_ != nullptr);
    if (!*pull_) {
        return;
    }

    Coroutine* previous = currentCoroutine_;
    currentCoroutine_ = this;
    try {
        (*pull_)();
        currentCoroutine_ = previous;
    }
    catch (...) {
        currentCoroutine_ = previous;
        throw;
    }
}
```

它完成三个动作：

```text
保存此前的当前协程
→ 把当前协程设置为自己
→ 调用 pull_ 切换上下文
→ 协程挂起、结束或抛异常后恢复此前协程
```

`pull_type` 的布尔状态表示协程是否还能继续。用户函数执行完成后 `!*pull_` 为真，后续 `resume()` 直接返回。

保存 `previous` 可以支持嵌套恢复：

```text
Coroutine A 正在运行
  → A 调用 B.resume()
  → current = B
  → B 挂起或结束
  → current 恢复为 A
```

异常路径也必须恢复 `previous`，否则线程会错误地认为失败的协程仍在运行。

## 9. yield() 只负责切回调用方

```cpp
void Coroutine::yield() {
    assert(push_ != nullptr);
    (*push_)();
}
```

调用 `push_()` 后：

```text
保存协程当前执行现场
→ 恢复调用 resume() 的现场
→ resume() 中的 pull_() 返回
→ resume() 恢复 previous 并返回
```

以后再次调用 `resume()`：

```text
resume() 设置 currentCoroutine_
→ pull_() 恢复协程
→ 上一次 push_() 返回
→ yield() 返回
→ 用户函数从 yield() 后继续
```

所有协程入口都经过 `resume()`，所有协程离开都会回到 `resume()`，所以当前协程只需由 `resume()` 统一维护，`yield()` 不再重复修改 `currentCoroutine_`。

## 10. current() 只提供动态上下文查询

```cpp
thread_local Coroutine* Coroutine::currentCoroutine_ = nullptr;

Coroutine* Coroutine::current() {
    return currentCoroutine_;
}
```

`thread_local` 让每个 EventLoop 线程拥有独立副本：

```text
线程 A：current = Coroutine A
线程 B：current = Coroutine B
没有协程运行：current = nullptr
```

它不是调度器，也不持有协程所有权，只回答“当前代码运行在哪个 Coroutine 中”。

## 11. EventLoop 负责恢复时机

Coroutine 只记录：

```cpp
EventLoop* loop_;
```

它不会自己启动 EventLoop，也不会监听 socket。`CoroutineChannel` 在发起 RPC 时验证当前协程属于自己的 EventLoop；响应到达后再将恢复任务投递回原 Loop：

```cpp
coroutineOwner->get_loop()->queue_in_loop([coroutineOwner]() {
    coroutineOwner->resume();
});
```

通过队列恢复而不是在 `on_read()` 中直接 `resume()`，可以避免在 Socket 读回调的调用栈内部重入业务协程。

# 完整 RPC 挂起与恢复流程

```text
EventLoop resume 业务 Coroutine
  → 业务调用 Protobuf Stub
  → CoroutineChannel::CallMethod()
  → 注册 sequenceId 对应的 completion
  → 请求进入非阻塞发送缓冲
  → Coroutine::yield()
  → 执行权回到 EventLoop

EventLoop 继续 epoll
  → socket 响应可读
  → Connection 拆出 Response Frame
  → CoroutineChannel 按 sequenceId 找到 PendingCall
  → 解析 response
  → queue_in_loop(resume Coroutine)

EventLoop 执行 resume
  → 上一次 yield() 返回
  → CallMethod() 检查错误并返回
  → Stub 返回
  → 业务代码继续使用 response
```

等待期间没有线程阻塞，业务代码却仍保持同步顺序。

# Result — 结果

- Boost.Context 保存和恢复真实执行现场，不需要手写 RPC 状态机；
- 有栈协程保留完整调用链和局部变量；
- `pull_ / push_` 被封装为更直观的 `resume() / yield()`；
- 初始挂起保证任务延迟到 `shared_ptr` 建立后执行；
- `currentCoroutine_` 支持当前协程查询、嵌套恢复和异常恢复；
- EventLoop 决定恢复时机，Coroutine 保持与网络、Frame 和 Protobuf 解耦。

单元测试覆盖：

- `yield()` 后从原位置继续；
- 用户函数异常后恢复当前协程指针；
- 内层协程挂起和结束后恢复外层当前协程。

# 权衡与边界

- Boost.Coroutine2 只提供用户态上下文切换，不会自动把阻塞系统调用变成非阻塞调用；网络层仍必须使用非阻塞 socket 和 EventLoop。
- Coroutine 没有抢占调度能力，任务必须在合适的位置主动 `yield()`。
- 当前没有取消接口；销毁挂起协程不等同于完成一套业务取消协议。
- 协程必须在所属 EventLoop 线程恢复，不能把同一执行上下文随意迁移到其他线程。
- RPC 场景依赖 `shared_from_this()`，因此 Coroutine 应通过 `shared_ptr` 创建。

# 面试核心问答

### 1. Boost.Coroutine2 有栈协程保存了什么？

它保存独立协程栈和恢复执行所需的上下文，包括栈指针、下一条指令位置及必要寄存器。恢复时重新加载这些状态，因此函数调用链和局部变量仍然存在。

### 2. pull_type 和 push_type 有什么区别？

`pull_type` 由调用方持有，用来进入或恢复协程；`push_type` 由协程函数获得，用来挂起并返回调用方。它们是同一条非对称协程的两个配对端点。

### 3. 为什么 push_ 保存一次就够？

它指向的是 Boost 为该协程创建的固定 push 端点，而不是某次 `yield()` 的代码地址。每次切换时 Boost 更新端点关联的内部上下文记录，端点对象本身的地址不变。

### 4. yield 的调用位置变了，下一次为什么还能恢复到正确位置？

恢复位置由 Boost.Context 在每次切换时保存的指令位置和栈状态决定，不由 `push_` 指针值决定。`push_` 只是发起切换的入口。

### 5. 为什么构造时要先 yield 一次？

构造 `pull_type` 会立即进入协程入口。初始 yield 阻止用户任务在 Coroutine 构造期间执行，使第一次 `resume()` 才真正启动任务，并保证外部 `shared_ptr` 已经建立。

### 6. 协程与线程有什么区别？

线程由操作系统调度并可能并行；当前协程在 EventLoop 线程内协作式切换，不会并行，也不会自动抢占。切换成本主要是用户态上下文保存和恢复。

### 7. 为什么使用 thread_local currentCoroutine_？

多个 EventLoop 线程可以各自执行协程。每线程独立的当前指针既避免全局竞争，也让 `CoroutineChannel::CallMethod()` 能找到当前线程正在运行的协程。

### 8. 为什么需要 shared_ptr？

RPC 挂起后，响应可能稍后才到达。completion 持有 Coroutine 的 shared_ptr，保证协程对象、独立栈和挂起中的 CallMethod 局部变量一直存活到恢复。

### 9. 为什么响应到达后不直接 resume？

直接恢复会在 Socket 读回调的调用栈中重入业务代码。投递到 EventLoop 队列后再恢复，可以让当前 I/O 回调先完成，控制流更稳定。

### 10. Boost 协程会自动处理阻塞 I/O 吗？

不会。Boost 只保存和恢复执行上下文。Tudou 仍需依靠非阻塞 socket、epoll 和 EventLoop，在网络未就绪时 yield，在就绪事件到达后 resume。

# 面试表达

> Tudou 使用 Boost.Coroutine2 封装有栈协程。Boost.Context 在用户态保存和恢复栈指针、指令位置和寄存器，使 RPC 调用链与局部变量可以跨网络等待保留。pull_type 是调用方恢复协程的端点，push_type 是协程挂起并返回调用方的端点；push_ 保存的是稳定端点对象，不是某次 yield 的代码地址，真正的恢复位置由 Boost 每次切换时更新的上下文记录决定。客户端发送请求后 yield 回 EventLoop，响应按 sequenceId 匹配后在原 Loop resume，因此业务代码保持同步写法，而 EventLoop 线程不会阻塞。
