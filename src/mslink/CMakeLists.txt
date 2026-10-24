cmake_minimum_required(VERSION 3.20)
project(mslink CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT MSVC)
    message(FATAL_ERROR "mslink 目前只支持 MSVC（Windows）")
endif()

# MSVC 默认按系统代码页(GBK)读源码，UTF-8 中文注释里的字节会被误判成续行符，
# 导致行号错位甚至吞掉代码 —— 必须显式声明 /utf-8（主项目同样做法）。
add_compile_options(/utf-8)

add_library(mslink_core STATIC src/relay.cpp)
target_include_directories(mslink_core PUBLIC include)
target_compile_definitions(mslink_core PUBLIC NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(mslink_core PUBLIC ws2_32)

add_executable(mslink src/main.cpp)
target_link_libraries(mslink PRIVATE mslink_core)

# 生成到 bin/ 下，方便与 Moonlight/Sunshine 一起打包
set_target_properties(mslink PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin")

include(CTest)
if (BUILD_TESTING)
    # These white-box tests include relay.cpp to exercise production helpers
    # and integration paths; do not also link mslink_core into these executables.
    add_executable(mslink_udp_packet_info_test tests/udp_packet_info_test.cpp)
    target_include_directories(mslink_udp_packet_info_test PRIVATE include)
    target_compile_definitions(mslink_udp_packet_info_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mslink_udp_packet_info_test PRIVATE ws2_32 bcrypt)
    add_test(NAME udp_reply_source COMMAND mslink_udp_packet_info_test)

    add_executable(mslink_queue_frame_test tests/relay_queue_frame_test.cpp)
    target_include_directories(mslink_queue_frame_test PRIVATE include)
    target_compile_definitions(mslink_queue_frame_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mslink_queue_frame_test PRIVATE ws2_32 bcrypt)
    add_test(NAME relay_queue_frame COMMAND mslink_queue_frame_test)

    add_executable(mslink_handshake_integration_test tests/relay_handshake_integration_test.cpp)
    target_include_directories(mslink_handshake_integration_test PRIVATE include)
    target_compile_definitions(mslink_handshake_integration_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mslink_handshake_integration_test PRIVATE ws2_32 bcrypt)
    add_test(NAME relay_handshake_integration COMMAND mslink_handshake_integration_test)
    set_tests_properties(relay_handshake_integration PROPERTIES TIMEOUT 120)

    add_executable(mslink_startup_test tests/relay_startup_test.cpp)
    target_include_directories(mslink_startup_test PRIVATE include)
    target_compile_definitions(mslink_startup_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mslink_startup_test PRIVATE ws2_32 bcrypt)
    add_test(NAME relay_startup COMMAND mslink_startup_test)
    set_tests_properties(relay_startup PROPERTIES TIMEOUT 120)

    add_executable(mslink_stream_open_failure_test tests/relay_stream_open_failure_test.cpp)
    target_include_directories(mslink_stream_open_failure_test PRIVATE include)
    target_compile_definitions(mslink_stream_open_failure_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mslink_stream_open_failure_test PRIVATE ws2_32 bcrypt)
    add_test(NAME relay_stream_open_failure COMMAND mslink_stream_open_failure_test)
    set_tests_properties(relay_stream_open_failure PROPERTIES TIMEOUT 120)

    add_executable(mslink_telemetry_test tests/relay_telemetry_test.cpp)
    target_include_directories(mslink_telemetry_test PRIVATE include)
    target_compile_definitions(mslink_telemetry_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    target_link_libraries(mslink_telemetry_test PRIVATE ws2_32 bcrypt)
    add_test(NAME relay_telemetry COMMAND mslink_telemetry_test)
    set_tests_properties(relay_telemetry PROPERTIES TIMEOUT 120)

    add_test(NAME relay_selftest COMMAND mslink selftest)
endif()
