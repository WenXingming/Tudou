cmake_minimum_required(VERSION 3.5)
project(Tudou)

# 启用 CTest 支持，允许在子目录中注册和运行测试
enable_testing()

# 统一设置 C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 公共头文件搜索路径（便于示例和子目录使用，方便代码里面包含头文件）
include_directories(${CMAKE_SOURCE_DIR}/src)
include_directories(${CMAKE_SOURCE_DIR}/lib)


# 打开 ASan/UBSan 重新编译跑 unitTest，这类“谁写坏了哪一块内存” ASan 很擅长直接给出栈回溯；
# ASan 已经把根因直接点出来了：这是 EventLoop::do_pending_functors 里的 use-after-free，会把 lambda（里面包含 connFd）的内存用坏，然后所有后面的问题都是它的“连锁反应”
# 但 ASan 会影响性能，平时编译不需要开启；仅在 Debug 模式下开启 ASan/UBSan：
# if (CMAKE_BUILD_TYPE STREQUAL "Debug")
#     set(SANITIZE_FLAGS "-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer")
#     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
#     set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
# endif()


# =============================================================================
# 添加 llhttp 静态库（源码引入方式。所以需要自己使用 add_library 命令编译生成库）
file(GLOB llhttp_source_files
    ${CMAKE_SOURCE_DIR}/lib/llhttp/*.c
)
add_library(llhttp STATIC ${llhttp_source_files})
# =============================================================================


# =============================================================================
# 添加 spdlog 库（该库为头文件库。所以无需编译生成库，只需包含头文件路径）
# =============================================================================


# =============================================================================
# 生成线程池库
file(GLOB threadpool_source_files
    ${CMAKE_SOURCE_DIR}/lib/threadpool/*.cpp
)
add_library(threadpool STATIC ${threadpool_source_files})
# =============================================================================


# =============================================================================
# 编译 tudou 生成库
file(GLOB tudou_source_files
    ${CMAKE_SOURCE_DIR}/src/tudou/*.cpp
    ${CMAKE_SOURCE_DIR}/src/tudou/http/*.cpp
    ${CMAKE_SOURCE_DIR}/src/base/*.cpp
)
add_library(tudou STATIC ${tudou_source_files})
target_link_libraries(tudou PRIVATE llhttp) # tudou 依赖 llhttp 库，其实也依赖 spdlog 库，但 spdlog 是头文件库，无需在这里 link
target_link_libraries(tudou PRIVATE threadpool) # tudou 依赖 threadpool 库
# =============================================================================


add_subdirectory(src/tudou/unitTest)
add_subdirectory(src/tudou/integrateTest)
add_subdirectory(examples)
