# Reactor 模式的 IO 多路复用

## 系统架构图

## Usage

## Benchmark

### 硬件配置

- CPU: Intel(R) Xeon(R) Silver 4214R CPU (12 Cores, 24 Threads)
- RAM: 64 GB
- Disk: SSD
- Network: localhost loopback interface
- Operating System: Ubuntu 22.04.5 LTS

### Wrk 测试

环境准备：

```bash
# git clone https://github.com/wg/wrk.git
# cd wrk
# make -j12
# 编译后 wrk 文件夹下会生成可执行文件 wrk，然后运行以下命令进行测试：
./wrk -t12 -c400 -d60s --latency http://127.0.0.1:8080
```

测试结果：

```bash
(base) wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t12 -c400 -d60s --latency http://127.0.0.1:8080
Running 1m test @ http://127.0.0.1:8080
  12 threads and 400 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    10.70ms   24.84ms 930.10ms   99.54%
    Req/Sec     3.55k   259.00    10.58k    97.70%
  Latency Distribution
     50%    9.31ms
     75%    9.41ms
     90%    9.59ms
     99%   10.30ms
  2539217 requests in 1.00m, 26.60GB read
Requests/sec:  42297.37
Transfer/sec:    453.68MB
```

测试结果显示，在 12 线程 + 400 并发连接下，1 分钟内总共处理了 2539217 个请求，读取了 26.60 GB 数据，具体性能指标如下：

- 响应时间（Latency）：
  - 平均响应时间：10.70 ms
  - 最大响应时间： 930.10 ms
  - 90% 请求的响应时间在 9.59 ms 以下
  - 99% 请求的响应时间在 10.30 ms 以下
- 吞吐量（Throughput）：
  - 每秒处理请求数（Requests/sec）：42297.37
  - 带宽（Transfer/sec）：453.68 MB/s

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