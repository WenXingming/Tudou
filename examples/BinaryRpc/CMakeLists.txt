protobuf_generate_cpp(ECHO_PROTO_SRCS ECHO_PROTO_HDRS echo.proto)

add_executable(binary-rpc-server server.cpp ${ECHO_PROTO_SRCS})
target_include_directories(binary-rpc-server PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(binary-rpc-server PRIVATE Tudou::tudou)

add_executable(binary-rpc-client client.cpp ${ECHO_PROTO_SRCS})
target_include_directories(binary-rpc-client PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(binary-rpc-client PRIVATE Tudou::tudou)
