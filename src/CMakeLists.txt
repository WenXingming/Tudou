add_library(tudou STATIC
    base/InetAddress.cpp
    tudou/http/HttpContext.cpp
    tudou/http/HttpRequest.cpp
    tudou/http/HttpResponse.cpp
    tudou/http/HttpServer.cpp
    tudou/http/Router.cpp
    tudou/http/SslContext.cpp
    tudou/http/TlsConnection.cpp
    tudou/net/Buffer.cpp
    tudou/net/Socket.cpp
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
)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(tudou
    PUBLIC
        llhttp::llhttp
    PRIVATE
        spdlog::spdlog
        Threads::Threads
        OpenSSL::SSL
        OpenSSL::Crypto
)
