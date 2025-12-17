cmake_minimum_required(VERSION 3.10)
project(TudouUnitTests)

# C++ 标准
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 多线程编译选项
set(THREADS_PREFER_PTHREAD_FLAG ON)

# =======================================================================
# GoogleTest：直接使用系统源码 /usr/src/googletest
# 避免链接 Anaconda 提供的 gtest 动态库，从而避免 libstdc++ 版本冲突
# =======================================================================
set(GTEST_SRC_DIR "/usr/src/googletest/googletest")

add_library(gtest STATIC
    ${GTEST_SRC_DIR}/src/gtest-all.cc
)
target_include_directories(gtest PUBLIC
    ${GTEST_SRC_DIR}
    ${GTEST_SRC_DIR}/include
)
find_package(Threads REQUIRED)
target_link_libraries(gtest PUBLIC Threads::Threads)

add_library(gtest_main STATIC
    ${GTEST_SRC_DIR}/src/gtest_main.cc
)
target_link_libraries(gtest_main PUBLIC gtest)


# ===========================================================
# Tudou 单元测试可执行文件（GoogleTest）
# 这里只专注于“真正的单元测试”
# ===========================================================

set(TUDOU_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/InetAddressTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/HttpContextTest.cpp
)

set(TUDOU_PROD_SOURCES
    ${CMAKE_SOURCE_DIR}/src/base/InetAddress.cpp
    ${CMAKE_SOURCE_DIR}/src/tudou/http/HttpContext.cpp
    ${CMAKE_SOURCE_DIR}/src/tudou/http/HttpRequest.cpp
)

add_executable(unitTest
    ${TUDOU_TEST_SOURCES}
    ${TUDOU_PROD_SOURCES}
)

target_include_directories(unitTest PRIVATE
    "${CMAKE_SOURCE_DIR}/src"   # Tudou 源码头文件（base/, tudou/http/ 等）
    "${CMAKE_SOURCE_DIR}/lib"   # llhttp 头文件
)

find_package(Threads REQUIRED)
target_link_libraries(unitTest PRIVATE
    gtest_main
    gtest
    llhttp
    Threads::Threads
)

add_test(NAME unitTest COMMAND unitTest)