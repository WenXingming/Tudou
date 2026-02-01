cmake_minimum_required(VERSION 3.10)
project(http_server)

# 设置 C++ 标准。设置 C++ 标准是一个好习惯，这能确保你的代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# CMake 不会自动解析通配符，因此必须先通过 file(GLOB ...) 获取文件列表
set(source_files_dir ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB source_files 
    ${source_files_dir}/*.cpp
)
add_executable(static-server ${source_files})


# ======================================================================================
# 使用库的几个步骤总结
# 1. target_include_directories()：添加 tudou 库头文件路径（编译时需要）
# 2. target_link_directories()：添加 tudou 库文件路径（链接时需要）。当自己编译生成库时，通常不需要这一步，因为 CMake 能自动找到库路径
# 3. target_link_libraries()：链接 tudou 静态库
# ======================================================================================


# ======================================================================================
# 添加库的头文件路径（编译时需要），因为代码中包含了 spdlog 头文件和 tudou 头文件
target_include_directories(static-server PRIVATE "${CMAKE_SOURCE_DIR}/libs/") 
target_include_directories(static-server PRIVATE
    "${CMAKE_SOURCE_DIR}/src"  # Tudou 源码头文件（base/, http/, tudou/ 等）
)
# 链接库
target_link_libraries(static-server PRIVATE 
    tudou
    ) 
# ======================================================================================
