file(GLOB source_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(StaticFileTcpServer ${source_files})

target_link_libraries(StaticFileTcpServer PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
