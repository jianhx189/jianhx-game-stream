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
