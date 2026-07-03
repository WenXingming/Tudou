# 该目标需要放入 muduo 工程中构建，所依赖的 target 已由 muduo 提供。
find_package(Threads REQUIRED)

add_executable(muduo-hello-benchmark main.cpp)
target_link_libraries(muduo-hello-benchmark PRIVATE muduo_net Threads::Threads)
