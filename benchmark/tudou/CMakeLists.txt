add_executable(tudou-hello-benchmark main.cpp)

target_link_libraries(tudou-hello-benchmark PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
