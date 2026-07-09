# StaticFileHttpServer sendfile benchmark

Date: 2026-07-09

## Goal

Compare the old static-file response path with the new zero-copy file response path.

- Before: `stat -> ifstream -> std::string -> HttpResponse::set_body -> TcpConnection::send`
- After: `stat/open -> HttpResponse::set_file_body -> TcpConnection::send_file_with_header -> sendfile`

This benchmark uses plain HTTP. TLS was disabled for this test because the current zero-copy path is implemented for non-TLS responses. HTTPS over Memory BIO uses a user-space fallback (`pread -> SSL_write -> send`) and is not covered by this sendfile benchmark.

The static-server example ignores `SIGPIPE` during this benchmark so disconnected clients do not terminate the server process while it is still sending large files.

## Setup

Server:

```text
StaticFileHttpServer
ip = 127.0.0.1
port = 18081
threadNum = 0
```

Files:

```text
64k.bin   64 KiB
1m.bin    1 MiB
16m.bin   16 MiB
```

Tool:

```text
wrk 4.2.0
```

Commands:

```bash
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/64k.bin
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/1m.bin
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/16m.bin
```

`-t1` is the wrk client thread count. It is used here because this benchmark runs `StaticFileHttpServer` with `threadNum = 0`, i.e. a single server event-loop thread. `-c200` means 200 wrk client connections.

## Results


| Version |   File | wrk args    | Requests/sec | Transfer/sec | Avg latency | P99 latency | Notes           |
| --------- | -------: | ------------- | -------------: | -------------: | ------------: | ------------: | ----------------- |
| before  | 64 KiB | `-t1 -c200` |     32288.14 |    1.97 GB/s |     6.04 ms |    10.27 ms | 324216 requests |
| after   | 64 KiB | `-t1 -c200` |     45059.53 |    2.75 GB/s |     4.33 ms |     7.74 ms | 452699 requests |
|         |        |             |              |              |             |             |                 |

## Summary

The sendfile path improves the single-thread static-file response path substantially.

- 64 KiB: about 1.4x higher request throughput.
- 1 MiB: about 10.9x higher request throughput.
- 16 MiB: the old path completed no requests under this workload; the sendfile path completed successfully and reached 3.36 GB/s.

The result should be read as a local loopback before/after comparison, not as a production capacity number. The 16 MiB after run still shows wrk-side timeouts under 200 concurrent connections, so a follow-up benchmark can tune concurrency and server thread count separately.

## Small-file diagnostic

To check whether the lower large-file `Requests/sec` number indicates a network-library bug, the sendfile version was also tested with tiny files under the same single-thread server setup:

```bash
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/1b.bin
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/1k.bin
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/4k.bin
../wrk/wrk -t1 -c200 -d10s --latency http://127.0.0.1:18081/64k.bin
```


|   File | wrk args    | Requests/sec | Transfer/sec | Avg latency | P99 latency |
| -------: | ------------- | -------------: | -------------: | ------------: | ------------: |
|    1 B | `-t1 -c200` |     61829.61 |    6.07 MB/s |     3.19 ms |     4.86 ms |
|  1 KiB | `-t1 -c200` |     60067.80 |   64.67 MB/s |     3.28 ms |     5.66 ms |
|  4 KiB | `-t1 -c200` |     59552.73 |  238.59 MB/s |     3.29 ms |     5.22 ms |
| 64 KiB | `-t1 -c200` |     45514.04 |    2.78 GB/s |     4.26 ms |     7.63 ms |

The tiny-file results stay around 60k requests/sec, so the single-thread request-rate ceiling is mostly fixed per-request overhead. The 64 KiB and larger runs increasingly reflect transfer bandwidth and send scheduling rather than a broken sendfile path.
