cmake_minimum_required(VERSION 3.10)
project(TudouIntegrationTests)

# 该目录只用于“人工运行”的集成测试，可执行程序不会注册到 ctest

# 线程库
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_executable(integrationTest
    main.cpp
    TestNetlib.cpp
)

# Tudou 顶层 CMake 已经定义了 llhttp / threadpool / tudou 静态库
# 这里只需要链接 tudou 和必要的依赖

target_link_libraries(integrationTest PRIVATE
    tudou
    threadpool
    llhttp
    Threads::Threads
)
