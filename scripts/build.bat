@echo off
rem ===========================================================================
rem  build.bat - configure + build with MSVC (Ninja generator)
rem  Usage:
rem     scripts\build.bat                          (build everything)
rem     scripts\build.bat --target ministream-selftest
rem     scripts\build.bat --target ministream-host ministream-client
rem  NOTE: ASCII only + CRLF line endings. cmd.exe reads .bat in the OEM
rem        codepage and mis-parses multi-line blocks with LF-only files.
rem ===========================================================================
setlocal

rem Fixed code page. CMake/Ninja may generate helper .bat files containing the
rem workspace path; if configure runs under code page 65001 (UTF-8), those files
rem are written as UTF-8 and cmd (936) reads the path as mojibake -> the build
rem fails/stalls at the "copy FFmpeg DLLs" step. Keep 936 for determinism.
chcp 936 >nul

set "ROOT=%~dp0.."
rem Force English toolchain output so CMake/Ninja can parse the /showIncludes prefix
set "VSLANG=1033"
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 (
    echo VCVARS_FAILED
    exit /b 1
)

set "CMAKE=D:\msys64\ucrt64\bin\cmake.exe"
set "NINJA=D:\msys64\ucrt64\bin\ninja.exe"
if not exist "%CMAKE%" (
    echo CMAKE_NOT_FOUND
    exit /b 1
)
if not exist "%NINJA%" (
    echo NINJA_NOT_FOUND
    exit /b 1
)

rem Concurrency guard: two ninja builds on one build dir look like a hang.
tasklist /fi "imagename eq ninja.exe" 2>nul | find /i "ninja.exe" >nul
if not errorlevel 1 (
    echo ANOTHER_BUILD_RUNNING
    echo   Another ninja is already running. Wait for it, or end ninja.exe first.
    exit /b 2
)

if not exist "%ROOT%\build\msvc" mkdir "%ROOT%\build\msvc"

rem A killed build can leave a stale .ninja_lock behind; with no ninja running
rem it only makes the next build wait forever.
if exist "%ROOT%\build\msvc\.ninja_lock" del /q "%ROOT%\build\msvc\.ninja_lock" 2>nul

echo [1/2] configure ...
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build\msvc" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DFFMPEG_ROOT="%ROOT%\third_party\ffmpeg"
if errorlevel 1 (
    echo CONFIGURE_FAILED
    exit /b 1
)

echo [2/2] build ...
"%CMAKE%" --build "%ROOT%\build\msvc" %*
if errorlevel 1 (
    echo BUILD_FAILED
    exit /b 1
)

echo BUILD_OK
exit /b 0
