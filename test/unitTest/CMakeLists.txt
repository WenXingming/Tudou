file(GLOB_RECURSE test_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS tudou/rpc/binary/test.proto)

add_executable(TudouUnitTest ${test_sources} ${PROTO_SRCS})
target_include_directories(TudouUnitTest PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_compile_features(TudouUnitTest PRIVATE cxx_std_17)

target_compile_definitions(TudouUnitTest PRIVATE
    TUDOU_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

find_package(GTest REQUIRED)

target_link_libraries(TudouUnitTest PRIVATE
    Tudou::tudou
    GTest::gtest
    GTest::gtest_main
)

add_test(NAME unitTest COMMAND TudouUnitTest)
