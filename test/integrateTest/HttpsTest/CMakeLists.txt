cmake_minimum_required(VERSION 3.10)

add_executable(https-test main.cpp)

target_link_libraries(https-test PRIVATE
    tudou
    llhttp
)
