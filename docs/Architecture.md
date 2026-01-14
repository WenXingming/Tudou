## 为什么选择 llhttp？

调研的 HTTP 协议相关库有很多，比如常见的有 `[http-parser](https://github.com/nodejs/http-parser)`、`[llhttp](https://github.com/nodejs/llhttp)`、`[picohttpparser](https://github.com/h2o/picohttpparser)、[cpp-httplib](https://github.com/yhirose/cpp-httplib)` 等等。我最终选择了 `llhttp`，主要基于以下几点考虑：

1. cpp-httplib 是一个完整的 HTTP 库，集成了网络通信和 HTTP 协议解析功能。我只想要一个高性能的 HTTP 协议解析库，因此排除了 cpp-httplib。（TODO: cpp-httplib 的设计和实现也值得学习借鉴，特别是其简洁的接口设计和易用性。）
2. http-parser 和 llhttp 都是 Node.js 官方使用的 HTTP 解析库。 llhttp 是 http-parser 的重写版本（后者的仓库已经归档），使用 C 语言编写，性能更高，支持 HTTP/1.x 和 HTTP/2 协议，并且设计更加现代化和模块化。llhttp 在性能测试中表现优异，能够满足高并发场景下的需求。
3. picohttpparser 是一个非常轻量级、高性能的 HTTP 解析库。然而，它的功能相对较少，缺乏对某些 HTTP 特性的支持，比如持久连接和分块传输编码等。
4. llhttp 性能优异、功能全面，且有良好的社区支持和文档资源，适合用于构建高性能的 HTTP 服务器。