@echo off
rem ===========================================================================
rem  build-map.bat - Release flags unchanged, only add /MAP.
rem  If the produced exe is byte-identical to a shipped exe, the .map file is
rem  ground truth for symbolicating that exe's crash dumps.
rem  NOTE: ASCII only.
rem ===========================================================================
setlocal
set "ROOT=%~dp0.."
set "VSLANG=1033"
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAILED & exit /b 1 )

set "CMAKE=D:\msys64\ucrt64\bin\cmake.exe"
set "NINJA=D:\msys64\ucrt64\bin\ninja.exe"

if not exist "%ROOT%\build\map" mkdir "%ROOT%\build\map"

"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build\map" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_EXE_LINKER_FLAGS="/MAP" ^
  -DFFMPEG_ROOT="%ROOT%\third_party\ffmpeg"
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )

"%CMAKE%" --build "%ROOT%\build\map" %*
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )

echo BUILD_OK
