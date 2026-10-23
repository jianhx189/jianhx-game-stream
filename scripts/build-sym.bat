@echo off
rem ===========================================================================
rem  build-sym.bat - same as build.bat but keeps PDB + MAP files.
rem  Used only for symbolizing crash dumps / post-mortem analysis.
rem  Output goes to build\sym so the normal build is untouched.
rem  NOTE: ASCII only. cmd.exe reads .bat in the OEM codepage.
rem ===========================================================================
setlocal
set "ROOT=%~dp0.."
set "VSLANG=1033"
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 ( echo VCVARS_FAILED & exit /b 1 )

set "CMAKE=D:\msys64\ucrt64\bin\cmake.exe"
set "NINJA=D:\msys64\ucrt64\bin\ninja.exe"

if not exist "%ROOT%\build\sym" mkdir "%ROOT%\build\sym"

rem  /O2 /Ob2 /DNDEBUG must match the Release build exactly so that RVAs line up
rem  with an already-shipped exe; /Zi only adds debug info.
"%CMAKE%" -S "%ROOT%" -B "%ROOT%\build\sym" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_CXX_FLAGS_RELEASE="/MD /O2 /Ob2 /DNDEBUG /Zi" ^
  -DCMAKE_EXE_LINKER_FLAGS="/DEBUG /INCREMENTAL:NO /MAP" ^
  -DFFMPEG_ROOT="%ROOT%\third_party\ffmpeg"
if errorlevel 1 ( echo CONFIGURE_FAILED & exit /b 1 )

"%CMAKE%" --build "%ROOT%\build\sym" %*
if errorlevel 1 ( echo BUILD_FAILED & exit /b 1 )

echo BUILD_OK
