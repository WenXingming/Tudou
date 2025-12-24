cmake_minimum_required(VERSION 3.10)
project(tcp_server_example)

# 设置 C++ 标准。设置 C++ 标准是一个好习惯，这能确保你的代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置多线程编译选项
set(THREADS_PREFER_PTHREAD_FLAG ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# CMake 不会自动解析通配符，因此必须先通过 file(GLOB ...) 获取文件列表
set(source_files_dir ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB source_files 
    ${source_files_dir}/*.cpp
)
add_executable(StaticFileTcpServer ${source_files})


# =============================================================================
# 使用 llhttp 库
target_include_directories(StaticFileTcpServer PRIVATE
    "${CMAKE_SOURCE_DIR}/libs"  # llhttp 库的头文件路径
)
# target_link_directories() -Ignore-
# 添加库文件路径非必须。因为 llhttp 是源码自定义编译生成库。cmake 编译后能够自动找到：build/src/test/libllhttp.a
target_link_libraries(StaticFileTcpServer PRIVATE llhttp) # 链接 llhttp 静态库
# =============================================================================


# =============================================================================
# 使用 threadpool 库
target_include_directories(StaticFileTcpServer PRIVATE
    "${CMAKE_SOURCE_DIR}/libs" # threadpool 库头文件路径
)
# target_link_directories() -Ignore-
target_link_libraries(StaticFileTcpServer PRIVATE threadpool) # 链接 线程池 静态库
# =============================================================================


# =============================================================================
# 使用 tudou 库
target_include_directories(StaticFileTcpServer PRIVATE
    "${CMAKE_SOURCE_DIR}/src"  # Tudou 源码头文件（base/, http/, tudou/ 等）
)
# 无需 target_link_directories。添加库文件路径非必须（因为 tudou 是源码引入方式。cmake 编译后能够自动找到：build/src/test/libllhttp.a）
target_link_libraries(StaticFileTcpServer PRIVATE tudou) # 链接 tudou 静态库
# =============================================================================


# =============================================================================
# 使用 spdlog 库（头文件引入方式，无需链接库文件）
target_include_directories(StaticFileTcpServer PRIVATE "${CMAKE_SOURCE_DIR}/libs/") # 添加库的头文件路径（编译时需要）
# target_link_directories() -Ignore- 添加库文件路径（pass）。头文件引入方式，没有库文件路径，无需添加
# target_link_libraries() -Ignore- 链接库（pass）。头文件引入方式，没有库文件。无需链接
# =============================================================================


# =============================================================================
# 链接 -lpthread。Linux 下 std::thread 依赖 pthread
find_package(Threads REQUIRED)
target_link_libraries(StaticFileTcpServer PRIVATE Threads::Threads)
# =============================================================================