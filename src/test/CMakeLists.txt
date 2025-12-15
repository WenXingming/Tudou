cmake_minimum_required(VERSION 3.10)
project(multi_io)

# 设置多线程编译选项
set(THREADS_PREFER_PTHREAD_FLAG ON)
# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 设置 C++ 标准。设置 C++ 标准是一个好习惯，这能确保你的代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


# 添加 llhttp 静态库（源码引入方式。所以需要自己使用 add_library 命令编译生成库）
file(GLOB llhttp_source_files
    ${CMAKE_SOURCE_DIR}/lib/llhttp/*.c
)
add_library(llhttp STATIC ${llhttp_source_files})


# 添加 spdlog 库（头文件引入方式。所以无需编译生成库，只需包含头文件路径）


# 编译生成可执行文件
set(source_dir_threadpool ${CMAKE_SOURCE_DIR}/lib/threadpool/)
set(source_dir_llhttp ${CMAKE_SOURCE_DIR}/lib/llhttp/)

set(source_dir_http ${CMAKE_SOURCE_DIR}/src/http/)
set(source_dir_tudou ${CMAKE_SOURCE_DIR}/src/tudou/)
set(source_dir_base ${CMAKE_SOURCE_DIR}/src/base/)
set(source_dir_test ${CMAKE_CURRENT_SOURCE_DIR}/) # test 目录

file(GLOB all_source_files #  CMake 不会自动解析通配符，因此必须先通过 file(GLOB ...) 获取文件列表
    ${source_dir_threadpool}/*.cpp
    ${source_dir_llhttp}/*.c

    ${source_dir_base}/*.cpp
    ${source_dir_http}/*.cpp
    ${source_dir_test}/*.cpp
    ${source_dir_tudou}/*.cpp
)

add_executable(unitTest ${all_source_files})


# 使用 llhttp 库
target_include_directories(unitTest PRIVATE "${CMAKE_SOURCE_DIR}/lib") # 添加库的头文件路径（编译时需要）
# target_link_directories(unitTest PRIVATE "${CMAKE_SOURCE_DIR}/lib")  # 添加库文件路径（非必须，因为 llhttp 是源码引入方式。cmake 编译后能够自动找到：build/src/test/libllhttp.a）
target_link_libraries(unitTest PRIVATE llhttp) # 链接 llhttp 静态库


# 使用 spdlog 库（头文件引入方式，无需链接库文件）
target_include_directories(unitTest PRIVATE "${CMAKE_SOURCE_DIR}/lib/") # 添加库的头文件路径（编译时需要）
# 添加库文件路径（pass）。头文件引入方式，没有库文件路径，无需 target_link_directories
# 链接库（pass）。头文件引入方式，没有库文件。无需 target_link_libraries

# 链接 -lpthread。Linux 下 std::thread 依赖 pthread
find_package(Threads REQUIRED)
target_link_libraries(unitTest PRIVATE Threads::Threads)