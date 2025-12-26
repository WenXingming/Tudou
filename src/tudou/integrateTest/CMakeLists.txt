# 该目录只用于“人工运行”的集成测试，可执行程序不会注册到 ctest

cmake_minimum_required(VERSION 3.10)
project(integrate_test)

# 统一设置 C++ 标准
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
add_executable(TudouIntegrateTest ${source_files})


# ======================================================================================
# 使用库的几个步骤总结
# 1. target_include_directories()：添加 tudou 库头文件路径（编译时需要）
# 2. target_link_directories()：添加 tudou 库文件路径（链接时需要）。当自己编译生成库时，通常不需要这一步，因为 CMake 能自动找到库路径
# 3. target_link_libraries()：链接 tudou 静态库
# ======================================================================================


# ======================================================================================
# 添加库的头文件路径（编译时需要）
target_include_directories(TudouIntegrateTest PRIVATE
    "${CMAKE_SOURCE_DIR}/src"
)
# 链接库
target_link_libraries(TudouIntegrateTest PRIVATE 
    tudou
)
# ======================================================================================
