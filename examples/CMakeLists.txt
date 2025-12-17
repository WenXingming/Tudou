cmake_minimum_required(VERSION 3.10)
project(server_example)

# 设置 C++ 标准。设置 C++ 标准是一个好习惯，这能确保你的代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置多线程编译选项
set(THREADS_PREFER_PTHREAD_FLAG ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()


# 编译生成可执行文件
# set(source_dir_threadpool ${CMAKE_SOURCE_DIR}/lib/threadpool/)
# set(source_dir_llhttp ${CMAKE_SOURCE_DIR}/lib/llhttp/) # 通过链接 llhttp 库使用，无需单独编译其源文件
# set(source_dir_http ${CMAKE_SOURCE_DIR}/src/http/)
# set(source_dir_tudou ${CMAKE_SOURCE_DIR}/src/tudou/)
# set(source_dir_base ${CMAKE_SOURCE_DIR}/src/base/)


# CMake 不会自动解析通配符，因此必须先通过 file(GLOB ...) 获取文件列表
set(source_files_dir ${CMAKE_SOURCE_DIR}/examples)
file(GLOB source_files 
    ${source_files_dir}/*.cpp
)
add_executable(server ${source_files})


# 使用 llhttp 库
target_include_directories(server PRIVATE
    "${CMAKE_SOURCE_DIR}/lib"  # llhttp / spdlog 等第三方库
)
# 无需 target_link_directories。添加库文件路径非必须（因为 llhttp 是源码引入方式。cmake 编译后能够自动找到：build/src/test/libllhttp.a）
target_link_libraries(server PRIVATE llhttp) # 链接 llhttp 静态库


# 使用 threadpool 库
target_include_directories(server PRIVATE
    "${CMAKE_SOURCE_DIR}/lib"  # llhttp / spdlog 等第三方库
)
# 无需 target_link_directories。添加库文件路径非必须（因为 threadpool 是源码引入方式。cmake 编译后能够自动找到：build/src/test/libllhttp.a）
target_link_libraries(server PRIVATE threadpool) # 链接 线程池 静态库


# 使用 tudou 库
target_include_directories(server PRIVATE
    "${CMAKE_SOURCE_DIR}/src"  # Tudou 源码头文件（base/, http/, tudou/ 等）
)
# 无需 target_link_directories。添加库文件路径非必须（因为 tudou 是源码引入方式。cmake 编译后能够自动找到：build/src/test/libllhttp.a）
target_link_libraries(server PRIVATE tudou) # 链接 tudou 静态库


# 使用 spdlog 库（头文件引入方式，无需链接库文件）
target_include_directories(server PRIVATE "${CMAKE_SOURCE_DIR}/lib/") # 添加库的头文件路径（编译时需要）
# 添加库文件路径（pass）。头文件引入方式，没有库文件路径，无需 target_link_directories
# 链接库（pass）。头文件引入方式，没有库文件。无需 target_link_libraries


# 链接 -lpthread。Linux 下 std::thread 依赖 pthread
find_package(Threads REQUIRED)
target_link_libraries(server PRIVATE Threads::Threads)