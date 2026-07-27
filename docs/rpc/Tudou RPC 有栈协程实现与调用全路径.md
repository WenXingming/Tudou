# Tudou RPC 有栈协程实现与调用全路径

## 1. 先建立一个正确直觉

Tudou 的协程是一个有栈协程（stackful coroutine）包装器：每个协程有自己的执行栈；执行到 `yield()` 时保存栈和寄存器状态并让出控制权；EventLoop 收到 RPC 响应后调用 `resume()`，协程从上次 `yield()` 后继续执行。

核心闭环：

```text
协程执行 → RPC 非阻塞发送 → yield()
    ↓
EventLoop/epoll 等待网络回包
    ↓
on_read() 根据 sequenceId 找到协程
    ↓
resume() 从 yield() 后继续
```

这里的“协程”不等于线程池，也不等于服务发现。它只解决：等待网络期间如何保留当前调用现场，又不阻塞承载 EventLoop 的 OS 线程。

---

## 2. 为什么需要协程

传统路径中，RPC 请求发出后：

```cpp
pendingRequests_[seq] = context;
send_request();
future.get();       // 挂起整个 OS 线程
use(response);
```

协程路径中：

```cpp
pendingRequests_[seq] = context;
write_request_nonblocking();
Coroutine::t_current_coroutine->yield(); // 只挂起用户态协程
use(response);                           // 回包后从这里继续
```

两者都在等待响应，但等待者不同：


| 路径   | 等待什么   | 回包后怎么恢复                 |
| -------- | ------------ | -------------------------------- |
| 线程版 | OS 线程    | `promise.set_value()` 唤醒线程 |
| 协程版 | 用户态协程 | EventLoop 调用`resume()`       |

所以协程主要改善高并发等待场景下的线程占用和调度开销，不保证单次网络延迟必然下降。

---

## 3. “有栈”到底是什么意思

普通函数的局部变量、返回地址、寄存器状态在 OS 线程栈上：

```cpp
void business() {
    int x = 1;
    rpc_call();
    use(x);
}
```

如果 `rpc_call()` 阻塞，整个线程都被卡住。有栈协程会给这段执行流一块独立的协程栈：

```text
OS 线程栈                 Coroutine A 栈
├─ EventLoop 回调帧       ├─ business() 的 x
├─ epoll 回调帧            ├─ rpc_call() 的返回位置
└─ resume() 调用帧         └─ yield() 保存的执行现场
```

`yield()` 不是从函数返回。它保存协程栈指针、返回地址和寄存器，然后切回调用 `resume()` 的外部执行流。之后再次 `resume()`，就能从原来的位置继续，`x` 和调用链都还在。

Tudou 使用 Boost.Coroutine2；Boost.Context 负责底层上下文切换，Tudou 自己决定何时 `yield/resume`。它没有额外调度线程，也不会自动 Hook 所有阻塞系统调用。

---

## 4. `Coroutine` 封装如何工作

### 4.1 pull/push 的直觉

```cpp
using coro_t = boost::coroutines2::coroutine<void>;
std::unique_ptr<coro_t::pull_type> pull_;
coro_t::push_type* push_ = nullptr;
```

可以这样记：

- 协程外部持有 `pull_type`，`(*pull_)()` 表示让协程继续运行；
- 协程函数内部得到 `push_type&`，`(*push_)()` 表示把控制权让回外部；
- Boost.Context 在两者切换时保存和恢复寄存器、栈指针及执行位置。

### 4.2 构造时为什么先 yield 一次

实际代码的关键部分：

```cpp
Coroutine::Coroutine(EventLoop* loop, std::function<void()> func)
    : loop_(loop), func_(std::move(func)) {
    pull_ = std::make_unique<coro_t::pull_type>(
        [this](coro_t::push_type& yield) {
            push_ = &yield;

            // 构造函数还未返回，先把控制权还回去
            (*push_)();

            Coroutine* saved = t_current_coroutine;
            t_current_coroutine = this;
            if (func_) {
                func_();
            }
            t_current_coroutine = saved;
        });
}
```

创建 `pull_type` 会立即进入 lambda。此时 `Coroutine` 对象可能还没有完成构造，外部的 `shared_ptr` 也还没有稳定建立。如果直接运行 `func_()`，其中调用 `shared_from_this()` 就有风险。

因此第一次 `(*push_)()` 的含义是：

```text
make_shared<Coroutine>()
  → 进入协程 lambda
  → push() 立即返回
  → Coroutine 构造完成
  → 外部第一次 resume()
  → 从 func_() 开始真正运行
```

### 4.3 resume 和 yield

```cpp
void Coroutine::resume() {
    if (pull_ && *pull_) {
        Coroutine* saved = t_current_coroutine;
        t_current_coroutine = this;
        (*pull_)();                 // 从上次暂停点继续
        t_current_coroutine = saved;
    }
}

void Coroutine::yield() {
    if (push_) {
        Coroutine* saved = t_current_coroutine;
        t_current_coroutine = nullptr;
        (*push_)();                 // 切回外部调用者
        t_current_coroutine = saved;
    }
}
```

`thread_local t_current_coroutine` 用来回答“当前代码是否运行在某个协程中”。`BinaryRpcChannel::CallMethod()` 根据它选择协程路径还是传统 `future.get()` 路径。

---

## 5. 一次 RPC 协程调用的完整时序

测试中的使用方式：

```cpp
EventLoop loop;
BinaryRpcChannel channel(&loop, "127.0.0.1", port);

auto coro = std::make_shared<Coroutine>(&loop, [&]() {
    TestEchoService_Stub stub(&channel);
    EchoRequest req;
    req.set_message("hello coroutine");
    EchoResponse resp;

    stub.Echo(nullptr, &req, &resp, nullptr);
    // 回包后，协程从这里继续
    EXPECT_EQ(resp.message(), "Echo: hello coroutine");
    loop.quit();
});

coro->resume();
loop.loop();
```

### 第一次 resume：运行到 RPC 等待点

```text
coro->resume()
  → func_() 开始
  → stub.Echo()
  → BinaryRpcChannel::CallMethod()
  → 创建 sequenceId 和 ResponseContext
  → pendingRequests_[seq] = context
  → 非阻塞写入请求
  → cur_coro->yield()
```

这里 `resume()` 调用返回了，但 `func_()` 没有结束。协程只是暂停，EventLoop 线程随后可以进入 `epoll_wait` 处理其他事件。

### EventLoop 收到回包：匹配并安排恢复

```cpp
void BinaryRpcChannel::on_read() {
    // 1. 非阻塞 read 到 readBuf_
    // 2. BinaryRpcCodec::decode 拆出完整帧
    uint64_t seq = respHeader.sequenceId;

    std::shared_ptr<ResponseContext> context;
    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        auto it = pendingRequests_.find(seq);
        if (it != pendingRequests_.end()) {
            context = it->second;
            pendingRequests_.erase(it);
        }
    }

    context->response->ParseFromString(respBodyRaw);

    EventLoop* loop = context->coroutine->get_loop();
    loop->run_in_loop([coro = context->coroutine]() {
        coro->resume();
    });
}
```

为什么要通过原来的 EventLoop 调度 `resume()`？因为协程应当在创建它的 EventLoop 线程中恢复。这样可以保持 Reactor 和协程的线程归属，不需要让协程跨线程切换。

### 第二次 resume：从 yield 后继续

```text
EventLoop → coro->resume()
  → 恢复协程栈和寄存器
  → 从 CallMethod() 内 yield() 的下一行继续
  → 检查 context->exception
  → CallMethod() 返回
  → stub.Echo() 返回
  → 业务代码读取 response
```

业务代码看起来像同步调用，但等待期间实际没有阻塞 EventLoop 线程。

---

## 6. 协程与单连接多路复用的关系

两者是两个维度：

```text
多路复用：多个请求如何共用一条 TCP 连接？
协    程：等待回包时，如何保留调用现场而不阻塞 OS 线程？
```

多路复用依靠 `sequenceId -> ResponseContext` 完成请求和响应匹配；协程只是让 `ResponseContext` 额外保存一个协程引用：

```cpp
struct ResponseContext {
    Message* response;
    std::promise<void> promise;       // 线程版使用
    std::shared_ptr<Coroutine> coroutine; // 协程版使用
    std::exception_ptr exception;
};
```

回包到达后：

- 线程版调用 `promise.set_value()`；
- 协程版把对应协程安排到原 EventLoop 中执行 `resume()`。

协程不是多路复用的必要条件；没有协程也可以用 `future.get()` 完成多路复用。

---

## 7. 错误、超时和连接断开

如果只在成功回包时 `resume()`，连接断开后协程会永久挂起。因此清理 pending 请求时也必须唤醒协程：

```cpp
void cleanup_pending_requests(const std::string& reason) {
    auto error = std::make_exception_ptr(
        std::runtime_error(reason));

    for (auto& [seq, context] : pendingRequests_) {
        if (context->coroutine) {
            context->exception = error;
            context->coroutine->get_loop()->run_in_loop(
                [coro = context->coroutine]() {
                    coro->resume();
                });
        }
    }
    pendingRequests_.clear();
}
```

协程恢复后，`CallMethod()` 检查 `context->exception` 并重新抛出，业务层可以正常使用 `try/catch`：

```cpp
try {
    stub.Echo(nullptr, &request, &response, nullptr);
} catch (const std::runtime_error& e) {
    // 连接关闭、协议错误等
}
```

这也是为什么 `shutdown()` 和 pending 清理都重要：前者让网络读循环退出，后者让所有等待者得到终结结果。

---

## 8. 这个实现的边界

- 协程不会自动创建线程；它运行在创建它的 EventLoop 线程上。
- 协程不会自动让出 CPU。若协程执行长时间计算且不 `yield()`，仍会阻塞 EventLoop。
- Boost.Coroutine2 不会自动把普通阻塞 `read()` 变成异步调用；这里依靠的是非阻塞 Socket + EventLoop。
- `resume()` 必须由正确的 EventLoop 调度，不能随意从其他线程直接恢复。
- 有栈协程保留的是调用栈现场，所以资源开销通常高于无栈状态机；它的优势是能够以接近同步的代码形态进行异步等待。

---

## 9. 面试时怎么讲

### 30 秒版本

> 我们用 Boost.Coroutine2 封装了有栈协程。RPC 发起时先把 `sequenceId` 和响应上下文放进 pending map，再通过非阻塞 Socket 发送，然后当前协程调用 `yield()`，保存自己的栈和寄存器状态，让出执行权给 EventLoop。EventLoop 收到回包后根据 `sequenceId` 找到对应上下文，并在原 EventLoop 中调用 `resume()`。协程从 `yield()` 后继续执行，所以业务代码看起来像同步调用，但等待网络时没有阻塞整条 OS 线程。连接断开时也会通过设置异常并恢复协程，避免永久挂起。

### 追问：为什么能从原地继续？

> 因为是有栈协程。`yield()` 保存协程栈指针、返回地址和寄存器上下文，`resume()` 恢复这些状态，所以可以从 `yield()` 后面的指令继续，而不是重新调用整个函数。

### 追问：协程创建了新线程吗？

> 没有。Boost.Coroutine2 只负责用户态上下文切换，协程运行在原来的 EventLoop 线程上；EventLoop 负责何时恢复它。

### 追问：协程和 future 有什么区别？

> `future.get()` 阻塞 OS 线程；`yield()` 只挂起用户态协程。两者都需要响应到达后唤醒等待者，但等待者和调度成本不同。

---

## 10. 最后形成这个心智模型

```text
协程 = 带独立用户栈的可暂停函数

resume()：从上次暂停点继续
yield() ：保存当前点并把控制权还给 EventLoop

RPC：
发送请求 → yield 等待
                  ↑
             收到回包后 resume
```

简历中的这句话可以准确理解为：

> “基于 Boost.Coroutine2 实现有栈协程 RPC 客户端”，就是用 Boost.Coroutine2 保存/恢复 RPC 调用的用户态执行上下文，用 EventLoop 驱动非阻塞网络，用回包事件恢复原协程，从而避免网络等待时占用一条阻塞的 OS 线程。
