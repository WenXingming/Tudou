# Reactor 模式的 IO 多路复用

## 系统架构图

## Usage

## Benchmark

硬件配置：

- CPU: Intel(R) Xeon(R) Silver 4214R CPU (12 Cores, 24 Threads)
- RAM: 64 GB
- Disk: SSD
- Network: localhost loopback interface
- Operating System: Ubuntu 22.04.5 LTS

----

### Wrk 测试

环境准备：

```bash
# git clone https://github.com/wg/wrk.git
# cd wrk
# make -j12
# 编译后 wrk 文件夹下会生成可执行文件 wrk，然后运行以下命令进行测试：
# ./wrk -t${线程数} -c${连接数} -d${测试时间}s --latency http://127.0.0.1:8080
./wrk -t12 -c400 -d60s --latency http://127.0.0.1:8080
```

测试结果（单 Reactor）：

```bash
(base) wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t1 -c400 -d60s --latency http://127.0.0.1:8080
Running 1m test @ http://127.0.0.1:8080
  1 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     8.17ms  501.57us  23.67ms   94.87%
    Req/Sec    48.57k     1.04k   50.18k    88.83%
  Latency Distribution
     50%    8.14ms
     75%    8.27ms
     90%    8.41ms
     99%    9.88ms
  2900309 requests in 1.00m, 30.38GB read
Requests/sec:  48305.13
Transfer/sec:    518.12MB
```

测试结果显示，在 1 线程 + 400 并发连接下，1 分钟内总共处理了 2900309 个请求，读取了 30.38 GB 数据，具体性能指标如下：

- 响应时间（Latency）：
  - 平均响应时间：8.17 ms
  - 最大响应时间： 23.67 ms
  - 90% 请求的响应时间在 8.41 ms 以下
  - 99% 请求的响应时间在 9.88 ms 以下
- 吞吐量（Throughput）：
  - 每秒处理请求数（Requests/sec）：48305.13
  - 带宽（Transfer/sec）：518.12 MB/s

这些结果表明该服务器在单 Reactor 模式下能够高效地处理大量并发请求，具有较低的响应时间和较高的吞吐量。

----

测试结果（多 Reactor），在开启 1 个 mainLoop 线程 + 16 个 ioLoop 线程后：

```bash
(base) wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t4 -c800 -d10s --latency http://127.0.0.1:8080
Running 10s test @ http://127.0.0.1:8080
  4 threads and 800 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.07ms  417.66us   5.71ms   70.15%
    Req/Sec   110.33k    21.31k  143.51k    68.25%
  Latency Distribution
     50%    1.02ms
     75%    1.25ms
     90%    1.61ms
     99%    2.18ms
  4390904 requests in 10.03s, 1.08GB read
Requests/sec: 437672.95
Transfer/sec:    110.61MB
```

测试结果显示，在 4 线程 + 400 并发连接下，10s 内总共处理了 4390904 个请求，读取了 1.08GB 数据，Requests/sec: 437672.95。

### Apache Bench 测试

环境准备：

```bash
# sudo apt-get update
# sudo apt-get install apache2-utils
# 然后运行以下命令进行测试：
ab -n 1000 -c 10 http://127.0.0.1:8080/
```

测试结果：

```bash
wxm@wxm-Precision-7920-Tower:~$ ab -n 1000 -c 10 -k -s 60 http://127.0.0.1:8080/
This is ApacheBench, Version 2.3 <$Revision: 1879490 $>
Copyright 1996 Adam Twiss, Zeus Technology Ltd, http://www.zeustech.net/
Licensed to The Apache Software Foundation, http://www.apache.org/

Benchmarking 127.0.0.1 (be patient)
apr_pollset_poll: The timeout specified has expired (70007)
```

通过 curl 命令可以看到服务器正确响应，不知道什么原因 ab 会报错，懒得管了...似乎是 ab 对 keep-alive 更加严格一些。

```bash
 curl -v http://127.0.0.1:8080/ -o /dev/null
```

## Citation

- 网络库：https://github.com/chenshuo/muduo
- Http 协议解析库：https://github.com/nodejs/llhttp
- 日志库：https://github.com/gabime/spdlog
- 压力测试工具：https://github.com/wg/wrk

## References

- 陈硕. 《Linux 多线程服务器编程：使用 muduo C++ 网络库》. 电子工业出版社, 2013.
- [muduo 源码剖析 - bilibili](https://www.bilibili.com/video/BV1nu411Q7Gq?spm_id_from=333.788.videopod.sections&vd_source=5f255b90a5964db3d7f44633d085b6e4)
- [llhttp 使用 - 知乎专栏](https://zhuanlan.zhihu.com/p/416575096)
- [spdlog 使用 - CSDN博客](https://blog.csdn.net/tutou_gou/article/details/121284474)
- [spdlog 使用](https://shuhaiwen.github.io/technical-documents/Documents/B-Programming%20Language/C%2B%2B/%E5%BC%80%E6%BA%90%E5%BA%93/spdlog/spdlog%E6%95%99%E7%A8%8B/)