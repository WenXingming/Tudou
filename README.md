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

Tudou 是一个基于 Reactor 模式的多线程 C++ 网络库，旨在构建高性能的网络服务器和应用程序。该库的主要特性包括：

1. **Reactor 模式**: 使用 Reactor 模式实现高效的事件驱动网络编程。
2. **多线程**: 支持多线程模型，提升并发处理能力。
3. **HTTP 协议支持**: 内置对 HTTP 协议的支持，方便构建 Web 服务器。
4. **高性能**: 通过优化的 I/O 处理和线程管理，实现高吞吐量和低延迟。
5. ...



## Benchmark: wrk 性能测试 ⚡

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

**单 Reactor测试结果**：

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

**多 Reactor测试结果**（开启 1 个 mainLoop 线程 + 16 个 ioLoop 线程）：

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

- 单元测试需要 Google Test 库支持（`sudo apt-get install libgtest-dev`）
- spdlog 日志库（已集成在 Tudou 中，无需额外安装）
- C++11 or higher
- CMake 3.10 or higher

## Usage 🎯

使用样例见 /examples。例如我使用 Tudou 编写了一个静态文件服务器 StaticFileHttpServer（详细代码见 /examples/StaticFileHttpServer）。How to use:

1. 编译项目（中的 StaticFileHttpServer 示例），生成可执行文件（StaticFileHttpServer）
2. 在 /etc 目录下创建配置文件目录结构，目录结构如下：

    ```bash
    static-file-http-server
      ├─ conf
      │  └─ server.conf
      ├─ html
      │  ├─ index.html
      │  ├─ xxx.html
      └─ log
         └─ server.log
    ```

    在 server.conf 只需要设置好自己的 IP 地址、端口号、启用的线程数，然后编译运行即可启动一个高性能的静态文件 HTTP 服务器，支持多线程处理请求。通过浏览器或 curl 访问对应的 URL 即可获取静态文件内容。

3. 配置文件目录路径可以不在 /etc 目录下，此时可以通过手动运行 StaticFileHttpServer 并在命令行终端中指定配置文件目录的路径

    ```bash
    ./StaticFileHttpServer /path/to/config/directory 
    # 如在用户主目录下的 static-file-http-server 目录下存放配置文件，则运行命令为：
    # ./StaticFileHttpServer /home/xxx/static-file-http-server
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
```
Tudou
├─ CMakeLists.txt
├─ README.md
├─ assets
│  ├─ nohup-1.out
│  └─ nohup-2.out
├─ configs
│  ├─ nginx
│  │  ├─ conf
│  │  │  ├─ conf.d
│  │  │  │  └─ default.conf
│  │  │  └─ nginx.conf
│  │  ├─ html
│  │  │  ├─ 50x.html
│  │  │  └─ index.html
│  │  └─ log
│  │     ├─ access.log
│  │     └─ error.log
│  └─ static-file-http-server
│     ├─ conf
│     │  └─ server.conf
│     ├─ html
│     │  ├─ happy-birthday.html
│     │  ├─ happy-christmas-script.js
│     │  ├─ happy-christmas-style.css
│     │  ├─ happy-christmas.html
│     │  ├─ homepage.html
│     │  └─ index.html
│     └─ log
│        └─ server.log
├─ docker-compose.yml
├─ docs
│  ├─ Architecture.mmd
│  ├─ Callback_Topic.mmd
│  ├─ Callback_Total.mmd
│  ├─ Document.md
│  ├─ Reactor.png
│  ├─ Reactor，高并发.png
│  └─ TcpServer_UML.mmd
├─ examples
│  ├─ StaticFileHttpServer
│  │  ├─ CMakeLists.txt
│  │  ├─ StaticFileHttpServer.cpp
│  │  ├─ StaticFileHttpServer.h
│  │  └─ main.cpp
│  └─ StaticFileTcpServer
│     ├─ CMakeLists.txt
│     ├─ StaticFileTcpServer.cpp
│     ├─ StaticFileTcpServer.h
│     └─ main.cpp
├─ libs
│  ├─ llhttp
│  │  ├─ api.c
│  │  ├─ api.h
│  │  ├─ http.c
│  │  ├─ llhttp.c
│  │  └─ llhttp.h
│  ├─ spdlog
│  │  ├─ async.h
│  │  ├─ async_logger-inl.h
│  │  ├─ async_logger.h
│  │  ├─ cfg
│  │  │  ├─ argv.h
│  │  │  ├─ env.h
│  │  │  ├─ helpers-inl.h
│  │  │  └─ helpers.h
│  │  ├─ common-inl.h
│  │  ├─ common.h
│  │  ├─ details
│  │  │  ├─ backtracer-inl.h
│  │  │  ├─ backtracer.h
│  │  │  ├─ circular_q.h
│  │  │  ├─ console_globals.h
│  │  │  ├─ file_helper-inl.h
│  │  │  ├─ file_helper.h
│  │  │  ├─ fmt_helper.h
│  │  │  ├─ log_msg-inl.h
│  │  │  ├─ log_msg.h
│  │  │  ├─ log_msg_buffer-inl.h
│  │  │  ├─ log_msg_buffer.h
│  │  │  ├─ mpmc_blocking_q.h
│  │  │  ├─ null_mutex.h
│  │  │  ├─ os-inl.h
│  │  │  ├─ os.h
│  │  │  ├─ periodic_worker-inl.h
│  │  │  ├─ periodic_worker.h
│  │  │  ├─ registry-inl.h
│  │  │  ├─ registry.h
│  │  │  ├─ synchronous_factory.h
│  │  │  ├─ tcp_client-windows.h
│  │  │  ├─ tcp_client.h
│  │  │  ├─ thread_pool-inl.h
│  │  │  ├─ thread_pool.h
│  │  │  ├─ udp_client-windows.h
│  │  │  ├─ udp_client.h
│  │  │  └─ windows_include.h
│  │  ├─ fmt
│  │  │  ├─ bin_to_hex.h
│  │  │  ├─ bundled
│  │  │  │  ├─ args.h
│  │  │  │  ├─ base.h
│  │  │  │  ├─ chrono.h
│  │  │  │  ├─ color.h
│  │  │  │  ├─ compile.h
│  │  │  │  ├─ core.h
│  │  │  │  ├─ fmt.license.rst
│  │  │  │  ├─ format-inl.h
│  │  │  │  ├─ format.h
│  │  │  │  ├─ os.h
│  │  │  │  ├─ ostream.h
│  │  │  │  ├─ printf.h
│  │  │  │  ├─ ranges.h
│  │  │  │  ├─ std.h
│  │  │  │  └─ xchar.h
│  │  │  ├─ chrono.h
│  │  │  ├─ compile.h
│  │  │  ├─ fmt.h
│  │  │  ├─ ostr.h
│  │  │  ├─ ranges.h
│  │  │  ├─ std.h
│  │  │  └─ xchar.h
│  │  ├─ formatter.h
│  │  ├─ fwd.h
│  │  ├─ logger-inl.h
│  │  ├─ logger.h
│  │  ├─ mdc.h
│  │  ├─ pattern_formatter-inl.h
│  │  ├─ pattern_formatter.h
│  │  ├─ sinks
│  │  │  ├─ android_sink.h
│  │  │  ├─ ansicolor_sink-inl.h
│  │  │  ├─ ansicolor_sink.h
│  │  │  ├─ base_sink-inl.h
│  │  │  ├─ base_sink.h
│  │  │  ├─ basic_file_sink-inl.h
│  │  │  ├─ basic_file_sink.h
│  │  │  ├─ callback_sink.h
│  │  │  ├─ daily_file_sink.h
│  │  │  ├─ dist_sink.h
│  │  │  ├─ dup_filter_sink.h
│  │  │  ├─ hourly_file_sink.h
│  │  │  ├─ kafka_sink.h
│  │  │  ├─ mongo_sink.h
│  │  │  ├─ msvc_sink.h
│  │  │  ├─ null_sink.h
│  │  │  ├─ ostream_sink.h
│  │  │  ├─ qt_sinks.h
│  │  │  ├─ ringbuffer_sink.h
│  │  │  ├─ rotating_file_sink-inl.h
│  │  │  ├─ rotating_file_sink.h
│  │  │  ├─ sink-inl.h
│  │  │  ├─ sink.h
│  │  │  ├─ stdout_color_sinks-inl.h
│  │  │  ├─ stdout_color_sinks.h
│  │  │  ├─ stdout_sinks-inl.h
│  │  │  ├─ stdout_sinks.h
│  │  │  ├─ syslog_sink.h
│  │  │  ├─ systemd_sink.h
│  │  │  ├─ tcp_sink.h
│  │  │  ├─ udp_sink.h
│  │  │  ├─ win_eventlog_sink.h
│  │  │  ├─ wincolor_sink-inl.h
│  │  │  └─ wincolor_sink.h
│  │  ├─ spdlog-inl.h
│  │  ├─ spdlog.h
│  │  ├─ stopwatch.h
│  │  ├─ tweakme.h
│  │  └─ version.h
│  └─ threadpool
│     ├─ Task.h
│     ├─ ThreadPool.cpp
│     └─ ThreadPool.h
└─ src
   ├─ base
   │  ├─ InetAddress.cpp
   │  ├─ InetAddress.h
   │  └─ NonCopyable.h
   └─ tudou
      ├─ Acceptor.cpp
      ├─ Acceptor.h
      ├─ Buffer.cpp
      ├─ Buffer.h
      ├─ Channel.cpp
      ├─ Channel.h
      ├─ EpollPoller.cpp
      ├─ EpollPoller.h
      ├─ EventLoop.cpp
      ├─ EventLoop.h
      ├─ EventLoopThread.cpp
      ├─ EventLoopThread.h
      ├─ EventLoopThreadPool.cpp
      ├─ EventLoopThreadPool.h
      ├─ TcpConnection.cpp
      ├─ TcpConnection.h
      ├─ TcpServer.cpp
      ├─ TcpServer.h
      ├─ http
      │  ├─ HttpContext.cpp
      │  ├─ HttpContext.h
      │  ├─ HttpRequest.cpp
      │  ├─ HttpRequest.h
      │  ├─ HttpResponse.cpp
      │  ├─ HttpResponse.h
      │  ├─ HttpServer.cpp
      │  └─ HttpServer.h
      ├─ integrateTest
      │  ├─ CMakeLists.txt
      │  ├─ TestNetlib.cpp
      │  ├─ TestNetlib.h
      │  └─ main.cpp
      └─ unitTest
         ├─ CMakeLists.txt
         ├─ HttpContextTest.cpp
         ├─ InetAddressTest.cpp
         └─ main.cpp

```