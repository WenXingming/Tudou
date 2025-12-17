# Tudou：一个 Reactor 模式的高性能网络框架⚡

```plain
  _______          _            
 |__   __|        | |           
    | | _   _   __| |  ___   _   _ 
    | || | | | / _` | / _ \ | | | |
    | || |_| || (_| || (_) || |_| |
    |_| \__,_| \__,_| \___/  \__,_|
```

## ✅ Introduction

Tudou 是一个基于 Reactor 模式的多线程 C++ 网络库，旨在构建高性能的网络服务器和应用程序。该库的主要特性包括：

1. **Reactor 模式**: 使用 Reactor 模式实现高效的事件驱动网络编程。
2. **多线程**: 支持多线程模型，提升并发处理能力。
3. **HTTP 协议支持**: 内置对 HTTP 协议的支持，方便构建 Web 服务器。
4. **高性能**: 通过优化的 I/O 处理和线程管理，实现高吞吐量和低延迟。
5. ...



## ⚡ Benchmark: wrk 性能测试

进行性能测试的硬件配置：

- CPU: Intel(R) Xeon(R) Silver 4214R CPU (12 Cores, 24 Threads)
- RAM: 64 GB
- Disk: SSD
- Network: localhost loopback interface
- Operating System: Ubuntu 22.04.5 LTS

---

wrk 下载编译：

```bash
# git clone https://github.com/wg/wrk.git
# cd wrk
# make -j12
# 编译后 wrk 文件夹下会生成可执行文件 wrk，然后运行以下命令进行测试：
# ./wrk -t${线程数} -c${连接数} -d${测试时间}s --latency http://127.0.0.1:8080
./wrk -t1 -c200 -d10s --latency http://127.0.0.1:8080
```

---

1. 单 Reactor测试结果：

   ```bash
   (base) wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t1 -c200 -d60s --latency http://192.168.3.3:8080
   Running 1m test @ http://192.168.3.3:8080
     1 threads and 200 connections
     Thread Stats   Avg      Stdev     Max   +/- Stdev
       Latency     2.86ms  101.67us   8.31ms   94.19%
       Req/Sec    69.45k   826.71    71.23k    83.17%
     Latency Distribution
        50%    2.86ms
        75%    2.89ms
        90%    2.92ms
        99%    2.99ms
     4147067 requests in 1.00m, 1.02GB read
   Requests/sec:  69102.97
   Transfer/sec:     17.46MB
   ```

   测试结果显示，在 **1 线程 + 200 并发连接下**，1 分钟内总共处理了 4147067 个请求，读取了 1.02 GB 数据，具体性能指标如下：
    - 响应时间（Latency）：
      - 平均响应时间：2.86 ms
      - 最大响应时间： 8.31 ms
      - 90% 请求的响应时间在 2.92 ms 以下
      - 99% 请求的响应时间在 2.99 ms 以下
    - 吞吐量（Throughput）：
      - 每秒处理请求数（Requests/sec）：69102.97

   这些结果表明该服务器在单 Reactor 模式下能够高效地处理大量并发请求，具有较低的响应时间和较高的吞吐量。

2. 多 Reactor测试结果（开启 1 个 mainLoop 线程 + 16 个 ioLoop 线程）：

   ```bash
   (base) wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t4 -c400 -d60s --latency http://192.168.3.3:8080
   Running 1m test @ http://192.168.3.3:8080
     4 threads and 400 connections
     Thread Stats   Avg      Stdev     Max   +/- Stdev
       Latency   547.37us  216.36us   4.24ms   67.65%
       Req/Sec   108.63k    20.95k  145.04k    73.54%
     Latency Distribution
        50%  509.00us
        75%  622.00us
        90%    0.98ms
        99%    1.10ms
     25935770 requests in 1.00m, 6.40GB read
   Requests/sec: 432144.30
   Transfer/sec:    109.21MB
   ```

    测试结果显示，在 **4 线程 + 400 并发连接下**，1 分钟内总共处理了 25935770 个请求，读取了 6.40 GB 数据，具体性能指标如下：
    - 响应时间（Latency）：
      - 平均响应时间：547.37 us
      - 最大响应时间：4.24 ms
      - 90% 请求的响应时间在 0.98 ms 以下
      - 99% 请求的响应时间在 1.10 ms 以下
    - 吞吐量（Throughput）：
      - 每秒处理请求数（Requests/sec）：432144.30

    这些结果表明该服务器在多 Reactor 模式下能够显著提升并发处理能力，响应时间进一步降低，吞吐量大幅提升，展示了良好的扩展性和高性能。

## 🔍 Requirements

- 单元测试需要 Google Test 库支持（sudo apt-get install libgtest-dev）
- spdlog 日志库（已集成在 Tudou 中，无需额外安装）
- C++11 or higher
- CMake 3.10 or higher

## 🎯 Usage

使用样例见 /examples。例如我使用 Tudou 编写了一个静态文件服务器 StaticFileHttpServer（详细代码见 /examples/StaticFileHttpServer）：

```cpp
/*
 * 静态文件 HTTP 服务器，用于测试 HttpServer：
 *   - 根据 URL 路径从指定根目录读取文件并返回
 *   - 例如：GET /hello-world.html -> <baseDir>/hello-world.html
 *   - 特殊规则："/" 映射为 "/index.html"（或者你可以根据需要修改）
 *   - 支持简单的文件内容缓存，提升性能
 */
     
#pragma once
#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>

#include "tudou/http/HttpServer.h"

class HttpServer;
class HttpRequest;
class HttpResponse;

class StaticFileHttpServer {
public:
    StaticFileHttpServer(const std::string& ip,
                         uint16_t port,
                         const std::string& baseDir,
                         int threadNum = 0);

    // 启动服务器（阻塞当前线程）
    void start();

private:
    void on_http_request(const HttpRequest& req, HttpResponse& resp); // 仅需设置消息处理回调即可
    std::string resolve_path(const std::string& urlPath) const;
    std::string guess_content_type(const std::string& filepath) const;
    bool get_file_content_cached(const std::string& realPath, std::string& content) const;

private:
    std::string ip_;
    uint16_t port_;
    std::string baseDir_;
    int threadNum_;

    std::unique_ptr<HttpServer> httpServer_;

    // 简单的文件内容缓存：避免每个请求都从磁盘读取同一个静态文件
    mutable std::mutex cacheMutex_;
    mutable std::unordered_map<std::string, std::string> fileCache_;
};

```

main.cpp：

```cpp
void run_static_http_server() {
    std::cout << "Starting HttpServer test..." << std::endl;

    std::string ip = "192.168.3.3";
    int port = 8080;
    std::string baseDir = "/home/wxm/Tudou/assets/";
    int threadNum = 16; // 0 表示使用单线程，大于 0 表示使用多线程

    StaticFileHttpServer server(ip, static_cast<uint16_t>(port), baseDir, threadNum);
    server.start();

    std::cout << "HttpServer test finished." << std::endl;
}

int main() {
    run_static_http_server();

    return 0;
}
```

访问 192.168.3.3:8080 即可看到静态文件服务器效果。

## 📚 Citation

- 网络库（muduo）：https://github.com/chenshuo/muduo
- Http 协议解析库（llhttp）：https://github.com/nodejs/llhttp
- 日志库（spdlog）：https://github.com/gabime/spdlog
- 单元测试框架（Google Test）：https://github.com/google/googletest
- 压力测试工具（wrk）：https://github.com/wg/wrk

## Others

- 陈硕. 《Linux 多线程服务器编程：使用 muduo C++ 网络库》. 电子工业出版社, 2013.
- [muduo 源码剖析 - bilibili](https://www.bilibili.com/video/BV1nu411Q7Gq?spm_id_from=333.788.videopod.sections&vd_source=5f255b90a5964db3d7f44633d085b6e4)
- [llhttp 使用 - 知乎专栏](https://zhuanlan.zhihu.com/p/416575096)
- [spdlog 使用 - CSDN博客](https://blog.csdn.net/tutou_gou/article/details/121284474)
- [spdlog 使用](https://shuhaiwen.github.io/technical-documents/Documents/B-Programming%20Language/C%2B%2B/%E5%BC%80%E6%BA%90%E5%BA%93/spdlog/spdlog%E6%95%99%E7%A8%8B/)