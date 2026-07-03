file(GLOB source_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(static-server ${source_files})

target_link_libraries(static-server PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
