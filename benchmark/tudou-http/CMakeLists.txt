cmake_minimum_required(VERSION 3.10)

add_executable(tudou-http-benchmark main.cpp)

target_include_directories(tudou-http-benchmark PRIVATE
    "${CMAKE_SOURCE_DIR}/libs"
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(tudou-http-benchmark PRIVATE tudou)