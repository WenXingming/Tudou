cmake_minimum_required(VERSION 3.10)
project(StarMind)

add_executable(StarMind
    main.cpp
    StarMindServer.cpp
)

target_link_libraries(StarMind PRIVATE tudou)

find_package(CURL REQUIRED)
target_link_libraries(StarMind PRIVATE CURL::libcurl)

# spdlog is header-only in this repo
