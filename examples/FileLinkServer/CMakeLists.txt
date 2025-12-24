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

# =============================================================================
# MySQL (mysql-connector-c++)
find_path(MYSQLCPPCONN_INCLUDE_DIR
    NAMES mysql_driver.h
)
find_library(MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn mysqlcppconn8
)

if (MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONN_LIBRARY)
    target_include_directories(FileLinkServer PRIVATE ${MYSQLCPPCONN_INCLUDE_DIR})
    target_link_libraries(FileLinkServer PRIVATE ${MYSQLCPPCONN_LIBRARY})
    target_compile_definitions(FileLinkServer PRIVATE FILELINK_WITH_MYSQLCPPCONN=1)
else()
    message(WARNING "mysql-connector-c++ not found; FileLinkServer will build without MySQL support. Install libmysqlcppconn-dev to enable.")
    target_compile_definitions(FileLinkServer PRIVATE FILELINK_WITH_MYSQLCPPCONN=0)
endif()

# =============================================================================
# Redis (hiredis)
find_path(HIREDIS_INCLUDE_DIR
    NAMES hiredis/hiredis.h
)
find_library(HIREDIS_LIBRARY
    NAMES hiredis
)

if (HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    target_include_directories(FileLinkServer PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(FileLinkServer PRIVATE ${HIREDIS_LIBRARY})
    target_compile_definitions(FileLinkServer PRIVATE FILELINK_WITH_HIREDIS=1)
else()
    message(WARNING "hiredis not found; FileLinkServer will build without Redis support. Install libhiredis-dev to enable.")
    target_compile_definitions(FileLinkServer PRIVATE FILELINK_WITH_HIREDIS=0)
endif()
