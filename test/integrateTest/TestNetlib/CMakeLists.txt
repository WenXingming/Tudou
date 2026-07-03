file(GLOB source_files CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

add_executable(TudouIntegrateTest ${source_files})
target_link_libraries(TudouIntegrateTest PRIVATE Tudou::tudou)
