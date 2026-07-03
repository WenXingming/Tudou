cmake_minimum_required(VERSION 3.25)

project(Tudou VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

option(TUDOU_BUILD_EXAMPLES "构建 Tudou 示例应用" OFF)
option(TUDOU_BUILD_BENCHMARKS "构建 Tudou 性能测试服务" OFF)
option(TUDOU_BUILD_TESTS "构建 Tudou 测试" OFF)

if(NOT TARGET llhttp::llhttp)
    find_package(llhttp 9.3 CONFIG QUIET)
endif()

if(NOT TARGET llhttp::llhttp)
    FetchContent_Declare(
        llhttp
        URL https://github.com/nodejs/llhttp/archive/refs/tags/release/v9.3.0.tar.gz
        URL_HASH SHA256=1a2b45cb8dda7082b307d336607023aa65549d6f060da1d246b1313da22b685a
    )

    # llhttp 使用通用构建选项，因此将其限制在函数作用域内，
    # 避免 Tudou 覆盖父项目的 BUILD_SHARED_LIBS 配置。
    function(tudou_fetch_llhttp)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_STATIC_LIBS ON)
        FetchContent_MakeAvailable(llhttp)
    endfunction()

    tudou_fetch_llhttp()
endif()

if(NOT TARGET spdlog::spdlog)
    find_package(spdlog 1.16 CONFIG QUIET)
endif()

if(NOT TARGET spdlog::spdlog)
    FetchContent_Declare(
        spdlog
        URL https://github.com/gabime/spdlog/archive/refs/tags/v1.16.0.tar.gz
        URL_HASH SHA256=8741753e488a78dd0d0024c980e1fb5b5c85888447e309d9cb9d949bdb52aa3e
    )

    function(tudou_fetch_spdlog)
        set(BUILD_SHARED_LIBS OFF)
        set(SPDLOG_BUILD_EXAMPLE OFF)
        set(SPDLOG_BUILD_TESTS OFF)
        set(SPDLOG_BUILD_BENCH OFF)
        set(SPDLOG_INSTALL OFF)
        FetchContent_MakeAvailable(spdlog)
    endfunction()

    tudou_fetch_spdlog()
endif()

add_subdirectory(src)

if(TUDOU_BUILD_EXAMPLES)
    add_subdirectory(examples/StaticFileHttpServer)
    add_subdirectory(examples/StarMind)
endif()

if(TUDOU_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

if(TUDOU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test/unitTest)
    add_subdirectory(test/integrateTest/TestNetlib)
    add_subdirectory(test/integrateTest/StaticFileTcpServer)
    add_subdirectory(test/integrateTest/HttpsTest)
endif()
