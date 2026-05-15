# Built in-tree as part of muduo project (targets already exist)
find_package(Threads REQUIRED)

add_executable(muduo-hello-benchmark main.cpp)
target_link_libraries(muduo-hello-benchmark PRIVATE muduo_net Threads::Threads)