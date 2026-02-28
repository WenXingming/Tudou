## Plan: Tudou 代码风格与可读性优化

**TL;DR** — 从底层 Epoll 到顶层 Router，逐层优化代码风格、注释、接口设计和封装。核心改动包括：统一命名规范、精简冗余注释（将"学习笔记"移至文档）、消除死代码、修正 `const_cast` 等代码异味、减少重复样板代码、统一使用 lambda 替代 `std::bind`，以及用 `std::make_unique` 替换 `new+reset` 模式。

---

### 一、全局风格统一（适用所有层）

1. **`this->` 使用不一致**：当前代码中有些地方用 `this->readCallback_`，有些直接用 `readCallback_`。统一去掉不必要的 `this->`，只在消歧义时使用（如 setter 参数同名时）。

2. **`std::bind` → lambda**：`Acceptor.cpp` 等处使用 `std::bind(&Acceptor::on_read, this, std::placeholders::_1)`，改为 `[this](Channel& ch) { on_read(ch); }`，更简洁易读。全局统一。

3. **`new` + `reset()` → `std::make_unique`**：如 `EventLoop.cpp` 的 `wakeupChannel_.reset(new Channel(this, wakeupFd_))` → `wakeupChannel_ = std::make_unique<Channel>(this, wakeupFd_)`。

4. **冗余 `std::move` on return**：如 `Buffer.cpp` 的 `return std::move(str)` 阻碍 NRVO，应直接 `return str`。同理 `EpollPoller.cpp` 和 `TcpConnection.cpp`。

5. **命名规范**：静态常量 `kNoneEvent_` 用了 Google Style 的 `k` 前缀又加了成员变量后缀 `_`，二者选其一。建议常量用 `kNoneEvent`（无后缀下划线），成员变量保留后缀 `_`。

6. **注释精简原则**：
   - 头文件注释保留 **「职责 + 线程安全说明 + 关键设计决策」**，删除冗长的实现细节描述
   - `.cpp` 内部注释保留 **关键 WHY**（如 `EventLoop.cpp` 的引用 bug 说明），删除 **WHAT**（如"从 fd 读数据到 readBuffer，然后触发上层回调处理数据"这类复述代码的注释）
   - 学习笔记类注释（如 `Channel.cpp` 长段解释 tie 机制原理）移至 `docs/Document.md`

---

### 二、Layer 1: Epoll 层 (Channel, EventLoop, EpollPoller)

**Step 1 — Channel**

1. **删除死代码 `index_`**：`Channel.h` 中 `index_` 及其 getter/setter 目前完全未被使用（muduo 用于标记 channel 在 poller 中的状态，但 Tudou 用 `channels_.find(fd)` 替代了）。删除 `set_index()`、`get_index()`、`index_` 成员。

2. **统一回调类型**：目前有 4 个完全相同签名的 type alias（`ReadEventCallback`/`WriteEventCallback`/`CloseEventCallback`/`ErrorEventCallback`），全部是 `std::function<void(Channel&)>`。合并为一个 `using EventCallback = std::function<void(Channel&)>`，四个成员变量类型都用它。

3. **`handle_events_with_guard` 中 EPOLLERR 处理后是否应该 return**：当前 EPOLLERR 执行 `handle_error_callback()` 后 `return`，但如果同时有 EPOLLIN+EPOLLERR，当前优先处理 error 并跳过 read。这是当前设计，保持即可，但加一行注释说明优先级逻辑。

4. **精简 `Channel.h` 注释**：头部 `@details` 块约 8 行，保留要点即可缩减为 3-4 行。删除 `or: typedef ...` 的旧语法提示。

**Step 2 — EpollPoller**

5. **解决 `data.fd` vs `data.ptr` 问题**：`EpollPoller.cpp` 注释说"不知道为什么一用 data.ptr 就会出错"。原因是 `epoll_event.data` 是 union，你在 `update_channel` 中同时设置了 `ev.data.fd = fd` 和 `ev.events`，如果改用 `ev.data.ptr = channel`，就不能再设置 `ev.data.fd`。修改方案：使用 `ev.data.ptr = static_cast<void*>(channel)` 替代 `ev.data.fd`，`get_activate_channels` 中直接 `static_cast<Channel*>(event.data.ptr)` 获取 channel，省去一次 hash map 查找。同时把 `EPOLL_CTL_ADD/DEL` 的 fd 参数改用 `channel->get_fd()`。

6. **`get_ready_num` 返回值处理**：当 `epoll_wait` 返回 -1 时（`EINTR` 中断），当前直接 log 后继续用 `-1` 作为 `numReady` 传给 `get_activate_channels`。应处理为 `if (numReady < 0) numReady = 0;` 避免后续异常。

7. **`poll()` 方法拆分是否必要**：`get_ready_num` → `get_activate_channels` → `dispatch_events` → `resize_event_list` 四步拆分有点过度。考虑合并 `get_activate_channels` + `dispatch_events` 为一步（遍历时直接 dispatch），减少一次 `vector<Channel*>` 的构造。

**Step 3 — EventLoop**

8. **`loop()` 参数 `timeoutMs` 没有被使用**：`EventLoop.cpp` 中 `poller_->poll(pollTimeoutMs_)` 始终使用静态成员，传入的参数被忽略。修复为 `poller_->poll(timeoutMs)`。

9. **`wakeupChannel_` 的回调签名不匹配**：`EventLoop.cpp` 中 `set_read_callback(std::bind(&EventLoop::on_read, this))` 把 `void()` 适配成 `void(Channel&)`，靠 `std::bind` 的参数丢弃特性工作。改为显式 lambda：`[this](Channel&) { on_read(); }`。

10. **`EpollPoller.h` 不应在 `EventLoop.h` 中 include**：`EventLoop.h` 直接 include 了 `EpollPoller.h`，导致上层也间接依赖 epoll 细节。改为前向声明 `class EpollPoller;`，include 放入 `.cpp`。

---

### 三、Layer 2: TcpServer 层

**Step 4 — Acceptor**

11. **接受连接信息的传递方式优化**：当前 Acceptor 通过成员变量 `acceptedConnFd_`/`acceptedPeerAddr_` 暂存，再由上层 `get_accepted_fd()` 取出。更直观的方式是回调签名改为 `std::function<void(int connFd, const InetAddress& peerAddr)>`，直接把参数传给 TcpServer，消除暂存成员变量。

12. **Acceptor 的 `on_error`/`on_close`/`on_write` 基本是空实现**：这些回调只有 log。可以考虑 Channel 允许回调为空（handle 时检查），Acceptor 不必设置无意义的回调。

**Step 5 — TcpConnection**

13. **`handle_close_callback` 中 `guardThis` 使用不明确**：创建了 `guardThis{shared_from_this()}` 但后面传 `closeCallback_(shared_from_this())` 又创建了一个。应改为 `closeCallback_(guardThis)` 复用已有的，或加 `[[maybe_unused]]` 标注。

14. **`send()` 跨线程安全**：TODO 提到需要 `run_in_loop`。建议加上：
    ```cpp
    if (!loop_->is_in_loop_thread()) {
        loop_->run_in_loop([self = shared_from_this(), msg]() { self->send_in_loop(msg); });
        return;
    }
    ```

**Step 6 — TcpServer**

15. **`handle_xxx_callback` 重复样板代码**：有 6 个 `handle_xxx_callback` 方法，模式完全一致（null 检查 + 调用）。抽取一个私有模板/辅助方法：
    ```cpp
    template<typename Cb, typename... Args>
    void invoke_if_set(const Cb& cb, const char* name, int fd, Args&&... args);
    ```

16. **`remove_connection` if/else 重复逻辑**：`run_in_loop` 分支和直接执行分支代码几乎一样。提取为 `do_remove_connection(int fd)` 内部方法，外层只管是否 `run_in_loop`。

**Step 7 — Buffer**

17. **Buffer 注释太细**：头文件注释中 ASCII art 示意图很好保留，但 `.cpp` 中每个函数的注释基本在复述代码。只保留 `read_from_fd` 和 `make_space` 的关键设计注释。

18. **`read_from_buffer()` 返回类型**：无参版本调用有参版本再 `std::move`，直接 `return read_from_buffer(readable_bytes())` 即可。

---

### 四、Layer 3: HTTP 层

**Step 8 — HttpResponse 接口增强**

19. **消除 `const_cast`**：`HttpServer.cpp` 中 `const_cast<HttpResponse::Headers&>(resp.get_headers())` 是因为缺少 `has_header()` 或非 const 的 `get_headers()` 方法。给 HttpResponse 添加 `bool has_header(const std::string& field) const` 和/或 `void set_header_if_absent(const std::string& field, const std::string& value)`，然后 `check_and_set_content_length` 简化为一行调用。

20. **`Headers` 类型别名**：HttpRequest 中 `Headers` 是 private，HttpResponse 中是 public。统一为 public，或者提取到 `HttpCommon.h` 中共享。

**Step 9 — HttpServer**

21. **`package_response_to_string` 是多余的包装**：只是 `return resp.package_to_string()`，直接在调用处内联，删除该方法。

22. **HTTP 和 TLS 职责分离**：当前 HttpServer 同时处理 HTTP 和 TLS 逻辑（约 100 行 TLS 代码），建议将 TLS 相关方法和成员提取到 `HttpsServer` 子类或 `TlsLayer` 组合类中，HttpServer 保持纯 HTTP。如果改动太大，至少用更清晰的代码区块分隔。

23. **`httpContexts_` 和 `tlsConnections_` 共享一把锁**：注释说共享 `contextsMutex_`，但两个 map 访问模式不同。如果未来性能优化，可考虑分离。当前阶段加注释说明即可。

**Step 10 — HttpContext**

24. **HttpContext 整体设计良好**，只需精简 `.cpp` 中的学习注释（如"并非每次调用都是新的 value"说明，保留但精简）。

---

### 五、Layer 4: Router 层

**Step 11 — Router**

25. **Router 代码质量最高**，注释风格好（解释 WHY 而非 WHAT），只需少量微调：
    - `starts_with` 在 C++20 中有 `std::string::starts_with`，加注释标注未来可替换
    - `build_allow_header` 可用 `std::string` 拼接替代 `std::ostringstream`，更轻量

---

### 六、文件级别整理

26. **将学习笔记类注释迁移到 `docs/Document.md`**：包括 tie 机制原理、`do_pending_functors` 的引用 bug 分析、epoll union 问题等。源码中留一行引用即可（`// See docs/Document.md#tie-mechanism`）。

27. **头文件依赖检查**：确保 `.h` 文件只 include 必要的头文件，其余用前向声明（尤其 `EventLoop.h` 不应直接 include `EpollPoller.h`）。

---

### Verification

- 每层改完后执行 `cd build && cmake .. && make -j$(nproc)` 确保编译通过
- 运行 `ctest` 执行已有测试
- 启动一个示例服务器（如 StaticFileHttpServer）验证功能正常
- 用 `curl` 测试 HTTP 响应正确性

### Key Decisions

- 选择 lambda 而非 `std::bind`：更可读、编译器更容易优化
- 选择通过回调参数传递 connFd 而非 Acceptor 暂存成员变量：更直观、无状态副作用
- 选择保留 `data.ptr` 优化而非继续用 `data.fd` + hash map：减少一次哈希查找
- 注释策略：源码只留精炼注释，原有学习笔记迁移到 docs
