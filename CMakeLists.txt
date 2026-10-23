# =============================================================================
#  MiniStream - 自用极简低延迟游戏串流工具
#  被控端(Host) + 远控端(Client)，Windows only
#
#  构建（MSVC）：
#     cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DFFMPEG_ROOT=D:/ffmpeg
#     cmake --build build --config Release
#
#  构建（Ninja + MSVC，需先 vcvars64.bat）：
#     cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFFMPEG_ROOT=D:/ffmpeg
#     cmake --build build
# =============================================================================
cmake_minimum_required(VERSION 3.20)
project(MiniStream VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# 输出目录统一，方便把两个 exe 和 DLL 放一起
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY        ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG  ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY        ${CMAKE_BINARY_DIR}/lib)

add_compile_definitions(
    _CRT_SECURE_NO_WARNINGS
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _WIN32_WINNT=0x0A00
    UNICODE
    _UNICODE
)

if(MSVC)
    add_compile_options(/utf-8 /W3 /MP /permissive- /Zc:__cplusplus)
    add_compile_options($<$<CONFIG:Release>:/O2>)
    # 关掉 MSVC 对 STL 的安全警告噪音
    add_compile_definitions(_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
    add_compile_options($<$<CONFIG:Release>:-O2>)
endif()

# -----------------------------------------------------------------------------
# FFmpeg：使用预编译 shared 包，需要 include/ 与 lib/
#   - 环境变量 FFMPEG_ROOT 或 -DFFMPEG_ROOT=<path>
#   - MSVC 用 gyan.dev 的 shared 包（含 .lib 导入库）
#   - MinGW 用 BtbN 的 win64-gpl-shared 包（含 .dll.a）
# -----------------------------------------------------------------------------
if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()
set(FFMPEG_ROOT "${FFMPEG_ROOT}" CACHE PATH "FFmpeg 预编译包根目录（包含 include/ 和 lib/）")

find_path(FFMPEG_INCLUDE_DIR
    NAMES libavcodec/avcodec.h
    HINTS "${FFMPEG_ROOT}/include" "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg/include")
find_library(FFMPEG_AVCODEC_LIB  NAMES avcodec  HINTS "${FFMPEG_ROOT}/lib")
find_library(FFMPEG_AVUTIL_LIB   NAMES avutil   HINTS "${FFMPEG_ROOT}/lib")
find_library(FFMPEG_SWSCALE_LIB  NAMES swscale  HINTS "${FFMPEG_ROOT}/lib")
find_library(FFMPEG_AVFORMAT_LIB NAMES avformat HINTS "${FFMPEG_ROOT}/lib")
find_library(FFMPEG_SWRESAMPLE_LIB NAMES swresample HINTS "${FFMPEG_ROOT}/lib")

if(NOT FFMPEG_INCLUDE_DIR OR NOT FFMPEG_AVCODEC_LIB OR NOT FFMPEG_AVUTIL_LIB)
    message(FATAL_ERROR
        "找不到 FFmpeg。请下载预编译包并指定 -DFFMPEG_ROOT=<路径>。\n"
        "  MSVC : https://www.gyan.dev/ffmpeg/builds/  -> ffmpeg-release-full-shared.7z\n"
        "  MinGW: https://github.com/BtbN/FFmpeg-Builds/releases -> ffmpeg-master-latest-win64-gpl-shared.zip\n"
        "  当前 FFMPEG_ROOT=${FFMPEG_ROOT}")
endif()
message(STATUS "FFmpeg include : ${FFMPEG_INCLUDE_DIR}")
message(STATUS "FFmpeg avcodec : ${FFMPEG_AVCODEC_LIB}")

# -----------------------------------------------------------------------------
# 公共静态库
# -----------------------------------------------------------------------------
file(GLOB MS_COMMON_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp)
add_library(ms_common STATIC ${MS_COMMON_SRC})
target_include_directories(ms_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIR})
target_link_libraries(ms_common PUBLIC
    ${FFMPEG_AVCODEC_LIB} ${FFMPEG_AVUTIL_LIB} ${FFMPEG_SWSCALE_LIB} ${FFMPEG_AVFORMAT_LIB}
    ${FFMPEG_SWRESAMPLE_LIB}
    ws2_32 winmm)

# Windows 系统库：DXGI 采集 / D3D11 渲染 / SendInput / 托盘 / Raw Input
set(MS_WIN_LIBS d3d11 dxgi d3dcompiler dwmapi shcore d2d1 dwrite bcrypt
                user32 gdi32 shell32 ole32 oleaut32 uuid advapi32 comctl32 version winmm ws2_32
                dbghelp)
if(MINGW)
    # MinGW 下这些名字由 mingw-w64 提供
    set(MS_WIN_LIBS ${MS_WIN_LIBS})
endif()
foreach(lib ${MS_WIN_LIBS})
    target_link_libraries(ms_common PUBLIC ${lib})
endforeach()

# -----------------------------------------------------------------------------
# 被控端 Host
#   服务端逻辑做成静态库，好让自检程序直接把「真实 HostService」跑起来做
#   生命周期回归（并发 stop / 会话中停止），而不是另写一个假实现。
# -----------------------------------------------------------------------------
file(GLOB MS_HOST_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/host/*.cpp)
set(MS_HOST_LIB_SRC ${MS_HOST_SRC})
list(FILTER MS_HOST_LIB_SRC EXCLUDE REGEX "main\\.cpp$")
add_library(ms_host STATIC ${MS_HOST_LIB_SRC})
target_link_libraries(ms_host PUBLIC ms_common)

add_executable(ministream-host WIN32 ${CMAKE_CURRENT_SOURCE_DIR}/src/host/main.cpp)
target_link_libraries(ministream-host PRIVATE ms_host)

# -----------------------------------------------------------------------------
# 远控端 Client
# -----------------------------------------------------------------------------
file(GLOB MS_CLIENT_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/client/*.cpp)
add_executable(ministream-client WIN32 ${MS_CLIENT_SRC})
target_link_libraries(ministream-client PRIVATE ms_common)

# -----------------------------------------------------------------------------
# 自检程序（控制台）：验证 SHA256 / HMAC / INI / 协议编解码 / 时钟
#   另外链接真实 HostService，做「并发 stop / 会话中停止」的生命周期回归。
# -----------------------------------------------------------------------------
add_executable(ministream-selftest tests/selftest.cpp)
target_link_libraries(ministream-selftest PRIVATE ms_host)

# 视频链路探针：真实采集 + 真实编码，把帧率/码率/编码延迟打出来。
# 不依赖任何网络与 UI，是排查"黑屏/卡顿/没有画面"的第一站。
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/video_probe.cpp")
    add_executable(ministream-videoprobe
        tests/video_probe.cpp
        src/host/capture.cpp
        src/host/encoder.cpp)
    target_link_libraries(ministream-videoprobe PRIVATE ms_common)
endif()

# -----------------------------------------------------------------------------
# 把 FFmpeg 运行库拷到输出目录，保证双击即可运行
#
# 【为什么在 configure 阶段拷，而不是 add_custom_target】
# 当前工作区路径含中文（D:\远程项目）。CMake 的 Ninja 后端会把"自定义命令"
# 写成 CMakeFiles\xxx.bat，而 .bat 的编码取决于**执行 configure 时的控制台
# 代码页**：在 UTF-8 页(65001)下生成的 .bat 会被 cmd(936) 读成乱码，于是
#   cd D:\杩滅▼椤圭洰\build\msvc  -> 系统找不到指定的路径
# 构建就卡/失败在"拷贝 FFmpeg DLL"这一步（曾经因此以为编译卡死）。
# 改用 configure 阶段的 file(COPY)：路径由 CMake 内部按 UTF-8 处理，不经过 .bat，
# 也不受控制台代码页影响。代价：换了 FFmpeg 目录/版本后要重新 configure。
# -----------------------------------------------------------------------------
if(FFMPEG_ROOT AND EXISTS "${FFMPEG_ROOT}/bin")
    file(GLOB FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
    if(FFMPEG_DLLS)
        file(COPY ${FFMPEG_DLLS} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
        list(LENGTH FFMPEG_DLLS FFMPEG_DLL_COUNT)
        message(STATUS "FFmpeg runtime DLLs -> ${CMAKE_RUNTIME_OUTPUT_DIRECTORY} "
                       "(${FFMPEG_DLL_COUNT} files, configure-time copy)")
    endif()
endif()

# -----------------------------------------------------------------------------
# 配置文件示例拷贝
# -----------------------------------------------------------------------------
add_custom_command(TARGET ministream-host POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/host.ini.example
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/host.ini)
add_custom_command(TARGET ministream-client POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/config/client.ini.example
            ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/client.ini)
