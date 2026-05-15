cmake_minimum_required(VERSION 3.10)

add_executable(tudou-hello-benchmark main.cpp)

target_include_directories(tudou-hello-benchmark PRIVATE
    "${CMAKE_SOURCE_DIR}/libs"
    "${CMAKE_SOURCE_DIR}/src"
)

target_link_libraries(tudou-hello-benchmark PRIVATE tudou)