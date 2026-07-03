add_executable(tudou-http-benchmark main.cpp)

target_link_libraries(tudou-http-benchmark PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
