cmake_minimum_required(VERSION 3.10)
project(unit_test)

# 统一设置 C++ 标准，确保代码在不同编译器上行为一致
# Ubuntu 24.04 的 Gtest 是 1.14.0 版本，需要 C++14 或更高标准。Ubuntu 24.04 的软件源只有这个版本，没有旧版本可选
set(CMAKE_CXX_STANDARD 17)
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
add_executable(TudouUnitTest ${source_files})

# ======================================================================================
# 添加库的头文件路径（编译时需要）
target_include_directories(TudouUnitTest PRIVATE
    "${CMAKE_SOURCE_DIR}/src" # tudou 源码头文件路径

)

# 链接 tudou
target_link_libraries(TudouUnitTest PRIVATE 
    tudou
)

# 链接 Google Test 库
find_package(GTest) # 现代 cmake 库。自动查找到 GTest 的头文件路径和库文件路径
if (NOT GTest_FOUND)
    message(WARNING "未找到 GTest，跳过 Tudou 单元测试 unitTest 的构建")
    return()
endif()
target_link_libraries(TudouUnitTest PRIVATE 
    gtest # Google Test 库
    gtest_main # main 函数入口（gtest_main 让我们不需要自己编写 main() 函数）
)

# 另一种使用 Google Test 的方法（手动指定 GTest 路径）
find_path(GTEST_INCLUDE_DIR 
    NAMES gtest/gtest.h)
find_library(GTEST_LIBRARY 
    NAMES gtest gtest_main)
if (GTEST_INCLUDE_DIR AND GTEST_LIBRARY)
    target_include_directories(TudouUnitTest PRIVATE ${GTEST_INCLUDE_DIR})
    target_link_libraries(TudouUnitTest PRIVATE ${GTEST_LIBRARY})
else()
    message(WARNING "未找到 GTest，跳过 Tudou 单元测试 unitTest 的构建")
    return()
endif()

# 注册单元测试到 ctest
add_test(NAME unitTest COMMAND TudouUnitTest)
# ======================================================================================