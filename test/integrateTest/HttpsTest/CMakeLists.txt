add_executable(https-test main.cpp)

target_link_libraries(https-test PRIVATE
    Tudou::tudou
    spdlog::spdlog
)
