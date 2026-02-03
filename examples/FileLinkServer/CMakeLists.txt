cmake_minimum_required(VERSION 3.10)
project(filelink_server)

# 设置 C++ 标准，确保代码在不同编译器上行为一致
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 设置编译类型为 Debug，以启用调试信息和关闭优化
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# CMake 不会自动解析通配符，因此必须先通过 file(GLOB ...) 获取文件列表
# 为了确保在添加或删除源文件时 CMake 能自动更新构建系统，使用 CONFIGURE_DEPENDS 选项
set(source_files_dir ${CMAKE_CURRENT_SOURCE_DIR})
file(GLOB source_files CONFIGURE_DEPENDS
    ${source_files_dir}/*.cpp
    ${source_files_dir}/auth/*.cpp
    ${source_files_dir}/metacache/*.cpp
    ${source_files_dir}/metastore/*.cpp
    ${source_files_dir}/filestore/*.cpp
)
add_executable(filelink-server ${source_files})

# OpenSSL (SHA-256 for content-addressed storage / dedup)
find_package(OpenSSL REQUIRED)
target_link_libraries(filelink-server PRIVATE OpenSSL::Crypto)

# ======================================================================================
# 使用库的几个步骤总结
# 1. target_include_directories()：添加 tudou 库头文件路径（编译时需要）
# 2. target_link_directories()：添加 tudou 库文件路径（链接时需要）。当自己编译生成库时，通常不需要这一步，因为 CMake 能自动找到库路径
# 3. target_link_libraries()：链接 tudou 静态库
# ======================================================================================


# ===============================================================================================
# ===============================================================================================
# 添加库的头文件路径（编译时需要），因为代码中包含了 spdlog 头文件和 tudou 头文件
target_include_directories(filelink-server PRIVATE "${CMAKE_SOURCE_DIR}/libs/")
target_include_directories(filelink-server PRIVATE "${CMAKE_SOURCE_DIR}/src")

# 链接库
# tudou 库
target_link_libraries(filelink-server PRIVATE
    tudou
)

# MySQL (mysql-connector-c++)
find_path(MYSQLCPPCONN_INCLUDE_DIR
    NAMES mysql_driver.h
)
find_library(MYSQLCPPCONN_LIBRARY
    NAMES mysqlcppconn mysqlcppconn8
)
if (MYSQLCPPCONN_INCLUDE_DIR AND MYSQLCPPCONN_LIBRARY)
    target_include_directories(filelink-server PRIVATE ${MYSQLCPPCONN_INCLUDE_DIR})
    target_link_libraries(filelink-server PRIVATE ${MYSQLCPPCONN_LIBRARY})
    target_compile_definitions(filelink-server PRIVATE FILELINK_WITH_MYSQLCPPCONN=1)
else()
    message(WARNING "mysql-connector-c++ not found; FileLinkServer will build without MySQL support. Install libmysqlcppconn-dev to enable.")
    target_compile_definitions(filelink-server PRIVATE FILELINK_WITH_MYSQLCPPCONN=0)
endif()

# Redis (hiredis)
find_path(HIREDIS_INCLUDE_DIR
    NAMES hiredis/hiredis.h
)
find_library(HIREDIS_LIBRARY
    NAMES hiredis
)
if (HIREDIS_INCLUDE_DIR AND HIREDIS_LIBRARY)
    target_include_directories(filelink-server PRIVATE ${HIREDIS_INCLUDE_DIR})
    target_link_libraries(filelink-server PRIVATE ${HIREDIS_LIBRARY})
    target_compile_definitions(filelink-server PRIVATE FILELINK_WITH_HIREDIS=1)
else()
    message(WARNING "hiredis not found; FileLinkServer will build without Redis support. Install libhiredis-dev to enable.")
    target_compile_definitions(filelink-server PRIVATE FILELINK_WITH_HIREDIS=0)
endif()
# ===============================================================================================
# ===============================================================================================
