
muduo 单线程性能测试：

```bash
wxm@wxm-Precision-7920-Tower:~/muduo$ ../wrk/wrk -t1 -c200 -d120s --latency http://0.0.0.0:8080
Running 2m test @ http://0.0.0.0:8080
  1 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.60ms  205.58us   7.77ms   96.41%
    Req/Sec   124.20k     6.01k  129.15k    93.58%
  Latency Distribution
     50%    1.58ms
     75%    1.62ms
     90%    1.66ms
     99%    2.93ms
  14828985 requests in 2.00m, 1.39GB read
Requests/sec: 123561.27
Transfer/sec:     11.90MB
```

Tudou 单线程性能测试：

```bash
wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t1 -c200 -d120s --latency http://0.0.0.0:8080
Running 2m test @ http://0.0.0.0:8080
  1 threads and 200 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.94ms  123.54us   6.34ms   95.42%
    Req/Sec   101.93k     2.90k  105.56k    86.92%
  Latency Distribution
     50%    1.94ms
     75%    1.97ms
     90%    2.02ms
     99%    2.15ms
  12171525 requests in 2.00m, 1.14GB read
Requests/sec: 101416.03
Transfer/sec:      9.77MB
```

muduo 多线程测试：

```bash
wxm@wxm-Precision-7920-Tower:~/muduo$ ../wrk/wrk -t10 -c1000 -d5s --latency http://0.0.0.0:8080
Running 5s test @ http://0.0.0.0:8080
  10 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     0.88ms  402.64us  17.42ms   74.69%
    Req/Sec    89.51k    13.21k  132.95k    87.80%
  Latency Distribution
     50%    0.93ms
     75%    1.06ms
     90%    1.20ms
     99%    2.01ms
  4452035 requests in 5.03s, 428.82MB read
Requests/sec: 884998.60
Transfer/sec:     85.24MB
```

tudou 多线程测试：

```bash
wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t10 -c1000 -d5s --latency http://0.0.0.0:8080
Running 5s test @ http://0.0.0.0:8080
  10 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.94ms  417.24us  13.76ms   75.51%
    Req/Sec    51.49k     3.23k   59.77k    74.80%
  Latency Distribution
     50%    1.96ms
     75%    2.15ms
     90%    2.27ms
     99%    3.23ms
  2561053 requests in 5.03s, 246.68MB read
Requests/sec: 509454.65
Transfer/sec:     49.07MB
```

无锁增加性能：

```bash
wxm@wxm-Precision-7920-Tower:~/Tudou$ ../wrk/wrk -t10 -c1000 -d5s --latency http://0.0.0.0:8080
Running 5s test @ http://0.0.0.0:8080
  10 threads and 1000 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     1.43ms  521.71us  34.82ms   89.62%
    Req/Sec    69.34k     4.91k   82.87k    72.00%
  Latency Distribution
     50%    1.50ms
     75%    1.59ms
     90%    1.66ms
     99%    3.03ms
  3449246 requests in 5.03s, 332.24MB read
Requests/sec: 686075.51
Transfer/sec:     66.08MB
```