# 该目录只用于“人工运行”的集成测试，可执行程序不会注册到 ctest

cmake_minimum_required(VERSION 3.10)
project(TudouIntegrationTests)

# 统一设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置多线程编译选项
set(THREADS_PREFER_PTHREAD_FLAG ON)


# 编译生成可执行文件
set(source_dir ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB source_files
    ${source_dir}/*.cpp
)
add_executable(integrationTest
    ${source_files}
)


# Tudou 顶层 CMake 已经定义了 llhttp / threadpool / tudou 静态库
# 这里只需要链接 tudou 和必要的依赖
find_package(Threads REQUIRED)
target_link_libraries(integrationTest PRIVATE
    tudou
    threadpool
    llhttp
    Threads::Threads
)
