cmake_minimum_required(VERSION 3.5)
project(Tudou)

# 打开 ASan/UBSan 重新编译跑 unitTest，这类“谁写坏了哪一块内存”ASan 很擅长直接给出栈回溯；
# ASan 已经把根因直接点出来了：这是 EventLoop::do_pending_functors 里的 use-after-free，会把 lambda（里面包含 connFd）的内存用坏，然后所有后面的乱七八糟 fd、崩溃都是它的“连锁反应”
# if (CMAKE_BUILD_TYPE STREQUAL "Debug")
#     set(SANITIZE_FLAGS "-fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer")
#     set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SANITIZE_FLAGS}")
#     set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZE_FLAGS}")
# endif()

add_subdirectory(src/test)
