# Tudou：一个 Reactor 模式的高性能网络框架 🚀

```Plaintext
  _______          _            
 |__   __|        | |           
    | | _   _   __| |  ___   _   _ 
    | || | | | / _` | / _ \ | | | |
    | || |_| || (_| || (_) || |_| |
    |_| \__,_| \__,_| \___/  \__,_|
    
```

## Introduction ✅

Tudou 是一个基于 Reactor 模式的多线程高性能 C++ 网络框架，旨在构建高性能的网络服务器和应用程序。该框架的主要特性包括：

1. **Reactor 模式**: 使用 Reactor 模式，结合 IO 多路复用技术（如 epoll），实现高效的事件驱动网络编程。
2. **多线程**: 支持多线程模型，提升并发处理能力。
3. **HTTP 协议支持**: 内置对 HTTP 协议的支持，方便构建 Web 服务器等。
4. **HTTP 路由功能**: 提供路由机制，支持精确匹配和前缀匹配，灵活地将客户端请求映射到对应的服务器处理逻辑上，实现请求的分发与处理。
5. **高性能**: 通过优化的 I/O 处理和线程管理，实现高吞吐量和低延迟。
6. ...

## Benchmark: wrk 性能测试 ⚡

进行性能测试的硬件配置：

- CPU: Intel(R) Xeon(R) Silver 4214R CPU (12 Cores, 24 Threads)
- RAM: 64 GB
- Disk: SSD
- Network: localhost loopback interface
- Operating System: Ubuntu 22.04.5 LTS

性能测试环境准备（wrk 下载编译）：

```bash
cd ~/ && git clone https://github.com/wg/wrk.git
cd wrk && make -j12
# 编译后 wrk 文件夹下会生成可执行文件 wrk，然后运行以下命令进行测试：
# ./wrk -t${线程数} -c${连接数} -d${测试时间}s --latency http://127.0.0.1:8080
# ./wrk -t1 -c200 -d10s --latency http://127.0.0.1:8080
```

---

**单 Reactor测试结果 🎢**：

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

----

**多 Reactor测试结果 🎢**（开启 1 个 mainLoop 线程 + 16 个 ioLoop 线程）：

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

## Requirements 🔍

- 单元测试需要 Google Test 库支持
    ```bash
    sudo apt-get update
    sudo apt-get install -y libgtest-dev
    ```
- llhttp HTTP 协议解析库（已集成在 Tudou 中，无需额外安装）
- spdlog 日志库（已集成在 Tudou 中，无需额外安装）
- C++11 or higher
- CMake 3.10 or higher

## Usage 🎯

使用样例见 `/examples`。

### 静态文件服务器示例 ✨

我使用 Tudou 编写了一个静态文件服务器 `StaticFileHttpServer`（详细代码见 `/examples/StaticFileHttpServer`）。使用方式如下：

1. 编译项目（中的 StaticFileHttpServer 示例），生成可执行文件（`StaticFileHttpServer`）
2. 在 `/etc` 目录下创建配置文件目录结构，目录结构如下：

    ```bash
    static-file-http-server # /etc 目录下的配置文件目录
      ├─ conf
      │  └─ server.conf
      ├─ html
      │  ├─ index.html
      │  ├─ xxx.html
      └─ log
         └─ server.log
    ```

    在 server.conf 只需要设置好自己的 IP 地址、端口号、启用的线程数，然后编译运行即可启动一个高性能的静态文件 HTTP 服务器，支持多线程处理请求。通过浏览器或 curl 访问对应的 URL 即可获取静态文件内容。server.conf 示例内容如下：

    ```yaml
    # Server IP and Port
    ip = 0.0.0.0
    port = 8080

    # Number of worker threads. 0 means only mainLoop thread is used.
    # More threads can handle more concurrent connections.
    threadNum = 1 # 1 mainLoop + 1 ioLoop (total 2 threads)
    ```

3. 配置文件目录路径可以不放在 /etc 目录下，此时可以通过手动运行 StaticFileHttpServer 并在命令行终端中指定配置文件目录的路径

    ```bash
    # 如在用户主目录下的 static-file-http-server 目录下存放配置文件，则运行命令为：
    # ./StaticFileHttpServer /home/xxx/static-file-http-server
    ./StaticFileHttpServer /path/to/config/directory 
    ```

### FileLink Server 示例 ✨

我使用 Tudou 实现了另一个功能：用户上传一个文件，后端将其组织存储，同时生成一个 URL 返回给前端，用户后续可以使用这个得到的 URL 访问或者下载该文件。

设计采用经典的客户端-服务器架构，前端通过 HTTP 协议与后端通信。后端使用 Tudou 实现高性能的 HTTP 服务器，处理文件上传和下载请求。设计采用 “元数据存数据库 + 文件实体存磁盘 + 热点数据存 Redis” 的经典架构。这种方式既能利用磁盘的大容量存储文件，又能利用数据库管理文件属性，同时利用 Redis 极大地提高文件索引速度。具体实现见 `/examples/FileLinkServer`。

环境要求：

1. 需要 MySQL 和 Redis 环境支持（若没有配置该环境则自动退化为无数据库和缓存模式）。可以使用 Docker 快速部署 MySQL 和 Redis 服务（见 docker-compose.yml）。
2. 需要安装 MySQL C++ Connector 库（`libmysqlcppconn-dev`）、hiredis 库（`libhiredis-dev`），以便能够使用 C++ 连接 MySQL 和 Redis 服务端进行操作。

    ```bash
    sudo apt-get update
    sudo apt-get install -y libmysqlcppconn-dev libhiredis-dev
    ```

## Citation 📚

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
