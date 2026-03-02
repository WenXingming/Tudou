cmake_minimum_required(VERSION 3.10)
project(Tudou)

# ===============================================================================================
# 设置 CMake 的构建并行级别，默认使用 CPU 核心数的 1/4 来编译，以提高构建速度，同时避免过度占用系统资源（例如爆内存）导致服务器失去响应
# C++ 编译非常吃内存，每个编译进程（cc1plus）通常消耗 1~2GB 内存。根据系统内存大小和 CPU 核心数，合理设置并行编译线程数可以显著提升构建速度，同时避免过度占用系统资源导致服务器失去响应。
# 在 4核/4G 服务器上，使用 2/4 的核心数进行编译都会使服务器失去响应
# 注意：set() 只在 CMake configure 阶段生效，而并行度是在 build 阶段 读取的，两个是不同的进程，所以不能直接在 CMakeLists.txt 中设置 CMAKE_BUILD_PARALLEL_LEVEL 来控制构建并行度
# include(ProcessorCount)
# ProcessorCount(NUM_PROCESSORS)
# if(NUM_PROCESSORS EQUAL 0)
#     set(NUM_PROCESSORS 1)
# endif()

# math(EXPR THREE_QUARTERS "(${NUM_PROCESSORS} * 1) / 4")
# if(THREE_QUARTERS EQUAL 0)
#     set(THREE_QUARTERS 1)
# endif()
# set(CMAKE_BUILD_PARALLEL_LEVEL ${THREE_QUARTERS})
# message(STATUS "System has ${NUM_PROCESSORS} CPU cores, using ${CMAKE_BUILD_PARALLEL_LEVEL} parallel build threads.")

# 1. 如果使用 VS Code 的 CMake Tools 扩展来构建，设置 VS Code 的 cmake.parallelJobs 配置项来控制构建并行度
# 2. 如果使用命令行构建，直接在构建命令中指定并行度，例如：cmake --build build -j1（这里的 -j1 表示使用 1 个线程进行并行构建）
# ===============================================================================================

# 启用 CTest 支持，允许在子目录中注册和运行测试（CTest 是 CMake 官方自带的测试驱动工具，用于收集和运行测试用例，本身不提供测试框架，只负责执行你注册的测试程序，并收集结果）
# 用 GTest 编写的单元测试，编译生成可执行文件后，然后用 add_test() 注册到 CTest 框架中运行
include(CTest)

# 统一设置 C++ 标准，确保代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()


# 打开 ASan/UBSan 重新编译跑，这类“谁写坏了哪一块内存” ASan 很擅长直接给出栈回溯；
# ASan 已经把根因直接点出来了：这是 EventLoop::do_pending_functors 里的 use-after-free，会把 lambda（里面包含 connFd）的内存用坏，然后所有后面的问题都是它的“连锁反应”
# 但 ASan 会影响性能，平时编译不需要开启；仅在 Debug 模式下开启 ASan/UBSan：
# if (CMAKE_BUILD_TYPE STREQUAL "Debug")
#     set(SANITIZE_FLAGS "-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer")
#     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
#     set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
# endif()


# 公共头文件搜索路径（所有 target 都包含，便于示例和子目录使用，方便代码里面包含头文件）
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/libs)


# ======================================================================================================
# ======================================================================================================
# 编译生成各个库（add_library）

# 编译生成 llhttp 静态库（源码引入方式。所以需要自己使用 add_library 命令编译生成库）
file(GLOB llhttp_source_files
    ${CMAKE_SOURCE_DIR}/libs/llhttp/*.c
)
add_library(llhttp STATIC ${llhttp_source_files})

# 添加 spdlog 库
# 该库为头文件库。所以无需编译生成库，只需包含头文件路径即可使用

# 生成线程池库
file(GLOB threadpool_source_files
    ${CMAKE_SOURCE_DIR}/libs/threadpool/*.cpp
)
add_library(threadpool STATIC ${threadpool_source_files})

# 编译 tudou 生成库
file(GLOB tudou_source_files
    ${CMAKE_SOURCE_DIR}/src/base/*.cpp
    ${CMAKE_SOURCE_DIR}/src/tudou/tcp/*.cpp
    ${CMAKE_SOURCE_DIR}/src/tudou/http/*.cpp
    ${CMAKE_SOURCE_DIR}/src/tudou/router/*.cpp
)
add_library(tudou STATIC ${tudou_source_files})

set(THREADS_PREFER_PTHREAD_FLAG ON) # 优先使用 pthread 选项，生成更兼容的代码
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)   # HTTPS 支持所需的 OpenSSL 库

target_link_libraries(tudou PRIVATE 
    llhttp
    threadpool # （实际没有使用到 threadpool 库）
    Threads::Threads
    OpenSSL::SSL      # TLS/SSL 协议实现
    OpenSSL::Crypto   # 加密原语
    ) 

# ======================================================================================================
# ======================================================================================================


# =============================================================================
# 使用库的几个步骤总结
# 1. target_include_directories()：添加 tudou 库头文件路径（编译时需要）
# 2. target_link_directories()：添加 tudou 库文件路径（链接时需要）。当自己编译生成库时，通常不需要这一步，因为 CMake 能自动找到库路径
# 3. target_link_libraries()：链接 tudou 静态库
# =============================================================================


# 递归构建子目录
add_subdirectory(examples/StaticFileHttpServer)
add_subdirectory(examples/FileLinkServer)
add_subdirectory(examples/StarMind)
add_subdirectory(test/unitTest)
add_subdirectory(test/integrateTest/TestNetlib)
add_subdirectory(test/integrateTest/StaticFileTcpServer)
add_subdirectory(test/integrateTest/HttpsTest)
