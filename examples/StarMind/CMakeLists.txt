add_executable(StarMind
    main.cpp
    ConfigLoader.cpp
    StarMindServer.cpp
)

find_package(CURL REQUIRED)

target_link_libraries(StarMind PRIVATE
    Tudou::tudou
    spdlog::spdlog
    CURL::libcurl
)
