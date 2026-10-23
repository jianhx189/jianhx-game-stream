@echo off
rem ===========================================================================
rem  check.bat - compile-check ONE translation unit (no link)
rem  Usage:  scripts\check.bat src\host\capture.cpp
rem  NOTE: keep this file ASCII-only. cmd.exe reads .bat as OEM codepage,
rem        so non-ASCII literals here would be mangled and executed as garbage.
rem ===========================================================================
setlocal
set "ROOT=%~dp0.."
set "VSLANG=1033"
call "D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if not exist "%ROOT%\build\objchk" mkdir "%ROOT%\build\objchk"
cl /nologo /c /EHsc /std:c++17 /W3 /permissive- /Zc:__cplusplus /utf-8 ^
   /D_CRT_SECURE_NO_WARNINGS /DWIN32_LEAN_AND_MEAN /DNOMINMAX /DUNICODE /D_UNICODE ^
   /I "%ROOT%\src" /I "%ROOT%\third_party\ffmpeg\include" ^
   /Fo"%ROOT%\build\objchk\\" %*
if errorlevel 1 (
  echo CHECK_FAILED
  exit /b 1
)
echo CHECK_OK
