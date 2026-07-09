file(GLOB source_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(jsonrpc-server ${source_files})

target_link_libraries(jsonrpc-server PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
