# ======================================================================================================
# Tudou 项目的顶层 CMake 配置文件，主要负责递归构建子目录中的代码，并设置一些全局的 CMake 配置
# ======================================================================================================

cmake_minimum_required(VERSION 3.10)
project(TUDOU)

# 统一设置 C++ 标准，确保代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# ===============================================================================================
# Tips：注意设置 CMake 的构建并行级别，避免过度占用系统资源（例如爆内存）导致服务器失去响应
# C++ 编译非常吃内存，每个编译线程可能消耗 1~2GB 内存，2 核 4G 服务器，-j4 可能爆内存系统卡死

# 1. 如果使用 VS Code 的 CMake Tools 扩展来构建，设置 VS Code 的 cmake.parallelJobs 配置项来控制构建并行度
# 2. 如果使用命令行构建，直接在构建命令中指定并行度，例如：cmake --build build -j1
# ===============================================================================================

# 启用 CTest 支持，允许在子目录中注册和运行测试
include(CTest)

# 打开 ASan/UBSan 重新编译跑，这类“谁写坏了哪一块内存” ASan 很擅长直接给出栈回溯；
# ASan 已经把根因直接点出来了：这是 EventLoop::do_pending_functors 里的 use-after-free，会把 lambda（里面包含 connFd）的内存用坏，然后所有后面的问题都是它的“连锁反应”
# 但 ASan 会影响性能，平时编译不需要开启；仅在 Debug 模式下开启 ASan/UBSan：
# if (CMAKE_BUILD_TYPE STREQUAL "Debug")
#     set(SANITIZE_FLAGS "-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer")
#     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
#     set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
# endif()

# 公共头文件搜索路径（所有 target 都包含，便于 examples 使用，方便代码里面包含头文件）
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/libs)

# 递归构建子目录
add_subdirectory(src)
add_subdirectory(examples/StaticFileHttpServer)
add_subdirectory(examples/FileLinkServer)
add_subdirectory(examples/StarMind)
add_subdirectory(test/unitTest)
add_subdirectory(test/integrateTest/TestNetlib)
add_subdirectory(test/integrateTest/StaticFileTcpServer)
add_subdirectory(test/integrateTest/HttpsTest)
