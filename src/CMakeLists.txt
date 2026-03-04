# ======================================================================================================
# 本 CMakeLists.txt 是为了编译生成 tudou 库（tudou.a）
# 注意：tudou 库依赖 llhttp 库，所以需要先编译 llhttp 库（源码引入方式）
# 注意：spdlog 是头文件库，无需编译生成库，只需包含头文件路径即可使用
# ======================================================================================================

cmake_minimum_required(VERSION 3.10)
project(Tudou)

# 统一设置 C++ 标准，确保代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# tudou 依赖 llhttp
file(GLOB llhttp_source_files CONFIGURE_DEPENDS
    ${CMAKE_SOURCE_DIR}/libs/llhttp/*.c
)
add_library(llhttp STATIC ${llhttp_source_files})

# 生成线程池库（暂未使用）
file(GLOB threadpool_source_files CONFIGURE_DEPENDS
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

# 添加搜索的头文件路径，编译时需要
target_include_directories(tudou PRIVATE ${CMAKE_SOURCE_DIR}/src)   # tudou 源码头文件路径
target_include_directories(tudou PRIVATE ${CMAKE_SOURCE_DIR}/libs)  # 第三方库头文件路径（如 llhttp、spdlog 等）

# 链接 tudou 依赖的库
set(THREADS_PREFER_PTHREAD_FLAG ON) # 优先使用 pthread 选项，生成更兼容的代码
find_package(Threads REQUIRED)      # std::thread 需要 pthread 库支持
find_package(OpenSSL REQUIRED)      # HTTPS 支持所需的 OpenSSL 库
target_link_libraries(tudou PRIVATE 
    llhttp
    threadpool          # 实际没有使用到 threadpool 库，可以删除这个依赖
    Threads::Threads
    OpenSSL::SSL        # TLS/SSL 协议实现
    OpenSSL::Crypto     # 加密原语
    ) 

# =============================================================================
# 使用库的几个步骤总结
#   1. target_include_directories()：添加 tudou 库头文件路径（编译时需要）
#   2. target_link_directories()：添加 tudou 库文件路径（链接时需要）。
#      当自己编译生成库时或能够 find_package 时，通常不需要这一步，因为 CMake 能自动找到库路径
#   3. target_link_libraries()：链接 tudou 静态库
# =============================================================================