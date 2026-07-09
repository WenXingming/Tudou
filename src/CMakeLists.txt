protobuf_generate_cpp(RPC_PROTO_SRCS RPC_PROTO_HDRS tudou/rpc/protocol/protocol.proto)

add_library(tudou STATIC
    tudou/tcp/InetAddress.cpp
    tudou/http/HttpContext.cpp
    tudou/http/HttpRequest.cpp
    tudou/http/HttpResponse.cpp
    tudou/http/HttpServer.cpp
    tudou/rpc/json/JsonRpcRouter.cpp
    tudou/rpc/json/JsonRpcServer.cpp
    tudou/rpc/json/JsonRpcClient.cpp
    tudou/rpc/protocol/RpcCodec.cpp
    tudou/rpc/protobuf/ProtobufRouter.cpp
    tudou/rpc/protobuf/ProtobufServer.cpp
    tudou/rpc/protobuf/ProtobufChannel.cpp
    ${RPC_PROTO_SRCS}
    tudou/http/TlsProbe.cpp
    tudou/http/HttpRouter.cpp
    tudou/http/TlsConfig.cpp
    tudou/http/TlsConnection.cpp
    tudou/tcp/Buffer.cpp
    tudou/tcp/Socket.cpp
    tudou/reactor/Channel.cpp
    tudou/reactor/EpollPoller.cpp
    tudou/reactor/EventLoop.cpp
    tudou/reactor/EventLoopThread.cpp
    tudou/reactor/EventLoopThreadPool.cpp
    tudou/tcp/Acceptor.cpp
    tudou/tcp/ConnectionHeartbeat.cpp
    tudou/tcp/TcpConnection.cpp
    tudou/tcp/TcpServer.cpp
    tudou/timer/Timer.cpp
    tudou/timer/TimerQueue.cpp
)

add_library(Tudou::tudou ALIAS tudou)

target_compile_features(tudou PUBLIC cxx_std_14)

target_include_directories(tudou
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}>
)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(tudou
    PUBLIC
        llhttp::llhttp
        nlohmann_json::nlohmann_json
        protobuf::libprotobuf
    PRIVATE
        spdlog::spdlog
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)
