# Channel tie 机制

> 参考书籍 p274，对应 muduo 同名设计。

## Situation（背景）

`TcpConnection` 由 `shared_ptr` 管理生命周期。`Channel` 是 `TcpConnection` 的成员，两者生命周期绑定，`TcpConnection` 析构时 `Channel` 随之析构。

`EventLoop` 驱动 `Channel::handle_events()` 分发事件，分发过程中可能触发**关闭连接**的回调链：

```
Channel::handle_events()
  └─ handle_events_with_guard()
       └─ handle_close_callback()          ← closeCallback 由此触发
            └─ TcpConnection::close_callback()
                 └─ TcpServer::remove_connection()
                      └─ connections_.erase(fd)  ← TcpConnection shared_ptr 引用计数归零
                           └─ ~TcpConnection()   ← Channel 随之析构！
```

**问题**：`handle_events_with_guard()` 尚未返回，栈帧仍在 `Channel` 方法内部，但 `Channel` 对象已被析构 → **悬空指针，段错误**。

## Task（任务）

在 `handle_events()` 整个执行期间，保证 `TcpConnection`（从而 `Channel`）不会被提前析构，同时不能引入循环引用或强制延长对象生命周期。

## Action（方案）

在 `Channel` 中增加一个 `weak_ptr<void>` 成员 `tie_` 和标志位 `isTied_`：

```cpp
// Channel.h
std::weak_ptr<void> tie_;   // 弱引用，不增加引用计数
bool isTied_;               // 是否已绑定（Acceptor 不绑定，TcpConnection 绑定）
```

**注册阶段**：`TcpConnection::connection_establish()` 调用 `tie_channel_to_owner()`，把自身的 `shared_ptr` 以弱引用形式存入 `Channel`：

```cpp
void TcpConnection::tie_channel_to_owner() {
    auto ptr = shared_from_this();
    channel_->tie_to_object(ptr);   // 存为 weak_ptr，不增加引用计数
}
```

**事件分发阶段**：`handle_events()` 进入时先将弱引用升级为临时 `shared_ptr guard`，在栈上持有一次强引用，保证分发期间对象不析构：

```cpp
void Channel::handle_events() {
    if (isTied_) {
        std::shared_ptr<void> guard = tie_.lock(); // 升级：栈上临时强引用
        if (guard) {
            handle_events_with_guard();            // 对象保活，安全执行
        }
    } else {
        handle_events_with_guard();                // Acceptor 走此分支，无需保活
    }
}
```

**为什么用 `isTied_` 标志**：  
`Acceptor` 不由 `shared_ptr` 管理，无法调用 `tie_to_object`，也没有"回调链中途析构"的风险（没有 `remove_connection` 类的回调），因此走无 tie 分支，避免对非 `shared_ptr` 对象做无意义的 `lock()`。

**为什么 `tie_` 用 `void*`**：  
`Channel` 位于框架底层，不应感知上层类型（`TcpConnection`），用 `void` 泛化，保持层次隔离。

## Result（效果）

- `guard` 在栈上持有临时强引用 → `handle_events_with_guard()` 执行期间引用计数 ≥ 1 → `TcpConnection` 不会析构 → `Channel` 不会析构 → 无悬空指针。
- `guard` 是局部变量，函数返回时自动释放强引用 → 不干扰正常的对象析构时序，无循环引用。
- `Acceptor` 不受影响，走 `else` 分支，无额外开销。

```
handle_events() 执行期间引用计数示意：
  TcpServer::connections_[fd] → shared_ptr  (ref=1)
  guard (栈上临时)             → shared_ptr  (ref=2)

handle_events() 返回后：
  guard 析构                               (ref=1)
  TcpServer 的 remove_connection 随后执行  (ref=0 → 析构)
```

---

## 补充：`weak_ptr<void>` 的详细用法

### 为什么 `void` 能正确管理生命周期

`shared_ptr<T>` 在构造时会把 deleter（即 `delete T` 或自定义删除器）存入**控制块**（control block）。控制块与类型无关，它通过类型擦除存储 deleter。当 `shared_ptr<void>` 的引用计数归零时，控制块调用之前存储的 deleter，正确销毁原始对象。

```
shared_ptr<TcpConnection> sp = ...;
shared_ptr<void> voidSp = sp;   // 隐式转换，控制块不变，deleter 仍然是 delete TcpConnection
// voidSp 离开作用域 → 引用计数-1 → 如果归零，控制块调用 delete TcpConnection ✓
```

`void` 擦除的是**解引用能力**（无法通过 `void*` 访问对象成员），但没有擦除**生命周期管理能力**。tie 机制恰好只需要后者——`guard` 的唯一作用是在栈上持有一次强引用，阻止引用计数归零，它本身从不访问对象。

### 既然不能从 `void` 转回去，回调如何访问对象

回调路径使用的是**建立 tie 时已保存的原始指针**，不经过 `guard`：

```
TcpConnection 构造时:
  Channel 持有 TcpConnection*  →  通过 set_xxx_callback 注册的 lambda 里捕获了 this
  事件分发时回调直接通过 this 访问 TcpConnection，不需要经过 guard

guard 的作用仅在于:
  保证 this（TcpConnection）在回调执行期间没有被析构
```

所以 `shared_ptr<void>` 负责保活（lifetime），`this` 负责访问（access），两者分工明确。

### 完整使用流程

**第 0 步：类型转换的原理**

`shared_ptr<T>` 到 `shared_ptr<void>` 的转换是 C++ 标准支持的隐式转换（`shared_ptr` 有模板构造函数和 `operator=`）。同理 `weak_ptr<T>` 也可隐式转换为 `weak_ptr<void>`。两者共享同一个控制块。

**第 1 步：TcpConnection 注册 tie**

```cpp
void TcpConnection::tie_channel_to_owner() {
    auto ptr = shared_from_this();               // shared_ptr<TcpConnection>  ref=1 (for count)
    channel_->tie_to_object(ptr);                // 隐式转为 shared_ptr<void>, 存入 weak_ptr<void> (ref 不增加)
}
// 注意: 这里的 ref=1 是指 shared_from_this 返回前的引用计数，不是 tie 增加的
```

**第 2 步：Channel 存储弱引用**

```cpp
void Channel::tie_to_object(const std::shared_ptr<void>& obj) {
    tie_ = obj;      // weak_ptr<void> = shared_ptr<void>: 不增加引用计数
    isTied_ = true;
}
```

**第 3 步：事件分发时升级为临时强引用**

```cpp
void Channel::handle_events() {
    if (isTied_) {
        std::shared_ptr<void> guard = tie_.lock();
        //  lock() 两种情况:
        //  - TcpConnection 还活着 → 返回 shared_ptr<void>, ref+1
        //  - TcpConnection 已析构 → 返回 nullptr
        if (guard) {
            handle_events_with_guard();  // 全程 ref>=2 (connections_+guard), 对象安全
        }
        // guard 离开作用域, 自动析构, ref-1
    } else {
        handle_events_with_guard();
    }
}
```

**第 4 步：回调链中可能触发 erase 但 tie 保护生效**

```
handle_events()
  guard lock 成功 (ref+1)
    handle_events_with_guard()
      handle_close_callback()
        TcpServer::remove_connection()
          connections_.erase(fd)       // 这一份 shared_ptr 释放, ref-1
                                       // 但 guard 还在栈上 → ref >= 1 → 不析构!
      // 返回到 handle_events_with_guard() → 栈帧仍然有效
  // 返回到 handle_events()
  guard 离开作用域, ref-1 → 此时才可能归零析构
```

### `weak_ptr` 使用方法总结

| 操作 | 含义 |
|---|---|
| `weak_ptr<T> w(sp)` | 从 shared_ptr 构造，**不增加引用计数** |
| `w = sp` | 赋值，同样不增加引用计数 |
| `auto sp = w.lock()` | 尝试升级为 shared_ptr：对象存活则返回非空 shared_ptr (ref+1)，已析构则返回 nullptr |
| `w.expired()` | 检查对象是否已析构（**不可靠**：多线程下返回值可能立即过期） |
| `w.use_count()` | 查看当前 shared_ptr 引用计数（仅调试用） |
| `w.reset()` | 清空 weak_ptr，不再引用任何对象 |

**典型模式：先 lock，持有返回值，再使用**

```cpp
auto guard = w.lock();  // ① lock 并用栈变量持有返回值
if (guard) {
    use(*guard);        // ② guard 在栈上，全程保活
}                        // ③ guard 离开作用域，自动释放强引用
```

> **为什么必须持有 lock() 的返回值**：`expired()` 检查完到实际使用之间，对象可能被其他线程析构。只有用栈上的 `shared_ptr` 变量承接 `lock()` 的返回值，才能保证使用期间对象不被析构。

### 关键陷阱

**陷阱 1：`expired()` 不是 `lock()` 的替代品**

```cpp
// 危险写法
if (!tie_.expired()) {
    tie_.lock();           // ← 不持有返回的 shared_ptr!
    handle_events_with_guard();  // 对象可能在 expired() 和这里之间被析构
}

// 正确写法
auto guard = tie_.lock();  // 必须用栈变量持有 lock() 的返回值
if (guard) {
    handle_events_with_guard();  // guard 在栈上, 全程保活
}
```

**陷阱 2：`shared_ptr<void>` 不能解引用**

```cpp
std::shared_ptr<void> guard = tie_.lock();
// guard.get() 返回 void*, 无法解引用
// 这是预期行为 —— tie 机制不需要通过 guard 访问对象
```

**陷阱 3：为什么不用 `shared_ptr` 而用 `weak_ptr`**

如果用 `shared_ptr` 存储 tie，会形成隐式的循环引用风险（Channel 持有 TcpConnection 的强引用），导致对象永远无法析构。`weak_ptr` 不增加引用计数，不会干扰正常的对象析构时序。
