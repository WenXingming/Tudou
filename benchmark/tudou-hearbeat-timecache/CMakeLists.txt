add_executable(tudou-hearbeat-timecache-benchmark main.cpp)

target_link_libraries(tudou-hearbeat-timecache-benchmark PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
