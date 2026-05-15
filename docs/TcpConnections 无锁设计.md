  之前是直接在消息回调中调用 conn->send()，现在改为通过 TcpServer::send(id, data) 来发送响应。这样符合模块化设计，但引入了额外的 mutex 加锁和 std::map 查找，可能会影响性能。

  之前的性能单线程能有 10w+ QPS，现在降到 3w+ QPS。
  
  补充说明：实际的性能瓶颈可能不止日志。当前 benchmark 通过 TcpServer::send(id, data)
  发送响应，这会在每条消息路径上执行 mutex 加锁 + std::map 查找。而 muduo 的 benchmark
  直接在消息回调中持有 TcpConnectionPtr，调用 conn->send()
  无需查找。如果关掉日志后仍有差距，这可能是值得关注的方向。

wxm@DESKTOP-QUAJHS3:~/wrk$ ../wrk/wrk -t1 -c100 -d10s --latency http://0.0.0.0:8080
Running 10s test @ http://0.0.0.0:8080
  1 threads and 100 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency     2.78ms    0.89ms   8.77ms   71.08%
    Req/Sec    31.86k     3.13k   39.61k    67.00%
  Latency Distribution
     50%    2.72ms
     75%    3.31ms
     90%    3.92ms
     99%    5.27ms
  317243 requests in 10.01s, 30.56MB read
Requests/sec:  31677.55
Transfer/sec:      3.05MB