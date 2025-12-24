cmake_minimum_required(VERSION 3.10)
project(filelink_server_example)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(THREADS_PREFER_PTHREAD_FLAG ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(source_files_dir ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB source_files
    ${source_files_dir}/*.cpp
    ${source_files_dir}/meta/*.cpp
    ${source_files_dir}/storage/*.cpp
    ${source_files_dir}/util/*.cpp
    ${source_files_dir}/cache/*.cpp
)

add_executable(FileLinkServer ${source_files})

# llhttp
target_include_directories(FileLinkServer PRIVATE "${CMAKE_SOURCE_DIR}/libs")
target_link_libraries(FileLinkServer PRIVATE llhttp)

# threadpool
target_include_directories(FileLinkServer PRIVATE "${CMAKE_SOURCE_DIR}/libs")
target_link_libraries(FileLinkServer PRIVATE threadpool)

# tudou
target_include_directories(FileLinkServer PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(FileLinkServer PRIVATE tudou)

# spdlog headers
target_include_directories(FileLinkServer PRIVATE "${CMAKE_SOURCE_DIR}/libs/")

find_package(Threads REQUIRED)
target_link_libraries(FileLinkServer PRIVATE Threads::Threads)

# 预留：后续你接入 MySQL / Redis client 时在这里加 link
# option(FILELINK_WITH_MYSQL "Enable MySQL metadata store" OFF)
# option(FILELINK_WITH_REDIS "Enable Redis cache" OFF)
