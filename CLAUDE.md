# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build & Test

```bash
# Configure and build (control parallelism to avoid OOM — C++ compilation is memory-heavy)
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2          # adjust -j based on available RAM

# Build a specific target
cmake --build build --target static-server
cmake --build build --target filelink-server
cmake --build build --target TudouUnitTest

# Run unit tests
cd build && ctest -R unitTest --output-on-failure

# Run a single test by name
cd build && ./test/unitTest/TudouUnitTest --gtest_filter=TestName*

# Enable ASan/UBSan for memory bug detection (uncomment the sanitizer block in CMakeLists.txt)
```

## Architecture

Tudou is a **multi-threaded Reactor network framework** in C++14, modeled after muduo. The core library compiles to a static library `tudou` under `src/`.

### Layer Stack (bottom-up, adjacent-class communication only)

```
EventLoop → EpollPoller (epoll wrapper)
EventLoop → Channel (fd + event callbacks, registers itself into Poller on construction)
Channel → Acceptor (listenFd: accept → new connection fd)
Channel → TcpConnection (connFd: read/write/close/error)
TcpServer holds Acceptor + ThreadPool + all TcpConnections
HttpServer holds TcpServer + per-connection HttpContext + TlsConnection
Router provides URL routing for HTTP dispatching
```

**Design rule**: Classes communicate only with direct neighbors. No skipping layers. This keeps the dependency graph manageable despite the inherently cyclic callback structure.

### Key Classes

| Class | Role |
|---|---|
| `EventLoop` | One per thread. Polls epoll, dispatches active channels, runs pending functors and timers |
| `EpollPoller` | `epoll_create/wait/ctl` wrapper; maintains fd→Channel* map |
| `Channel` | Owns fd lifecycle (RAII: creates→registers, destructs→deregisters+closes fd). Event callbacks: read/write/close/error |
| `Acceptor` | Accepts new connections on listenFd, reports via `NewConnectCallback(int connFd, InetAddress& peer)` |
| `TcpConnection` | Per-connection state: fd, Channel, read/write buffers, callbacks. Managed by `shared_ptr` |
| `TcpServer` | Accepts connections, assigns them to IO threads, manages connection table |
| `HttpServer` | HTTP/HTTPS facade. Holds a TcpServer, per-connection HttpContexts, optional TLS state |
| `HttpContext` | Wraps llhttp parser + parsed HttpRequest per connection |
| `TimerQueue` | timerfd-based timer management, owned by EventLoop |
| `Buffer` | Netty-style read/write buffer with `vector<char>` backing |

### Event Loop Thread Model

- **Main loop thread**: runs Acceptor (accepts new connections)
- **IO loop threads** (configurable count, 0 = single-threaded): run per-connection I/O
- `EventLoopThreadPool` manages the pool. New connections are distributed round-robin to IO loops.
- `one loop per thread` enforced via `thread_local static EventLoop*`

### Lifecycle Management Rules (from docs/Document.md)

1. **Ownership**: smart pointers (`unique_ptr` for exclusive, `shared_ptr` for shared ownership)
2. **Access**: raw pointers or references — no ownership transfer. `EpollPoller` holds `Channel*`, not `shared_ptr<Channel>`.
3. `TcpConnection` uses `shared_ptr` because its lifecycle is complex (callbacks may extend its lifetime). The **tie mechanism** (`Channel::tie_to_object`) prevents Channel from being destroyed mid-callback by holding a `weak_ptr` to its owning TcpConnection.
4. When using shared_ptr in maps (e.g., `HttpContexts`), copy the shared_ptr inside the lock, then use it outside the lock to keep the critical section minimal.

### Callback Pattern

Callbacks are the primary cross-class communication mechanism (bottom-up). A class exposes `set_xxx_callback(Callback cb)` for its upper neighbor to register. The lower class triggers the callback when events occur. Look for:
- `set_xxx_callback` / `xxxCallback_` member — registration point
- `handle_xxx()` or `notify_xxx_callback()` — trigger point
- The upper neighbor's `xxx_callback()` or `on_xxx()` — actual handler implementation

### Third-Party Libraries (vendored in `libs/`)

- **llhttp**: HTTP parser (C library, compiled as static lib)
- **spdlog**: Header-only logging library
- **threadpool**: Included but not currently used by the framework

### Examples

- `StaticFileHttpServer` — static file HTTP(S) server
- `FileLinkServer` — file upload/download service (MySQL + Redis metadata)
- `StarMind` — AI chat service (calls external LLM API via libcurl)

Each example uses a config directory (`-r /path/to/config`) with `conf/server.conf` + `assets/` + `log/`.
