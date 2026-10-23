@echo off
rem ============================================================
rem  Classroom Bypass Tool - build bypass.exe (64-bit GUI)
rem
rem  Usage:  build_bypass.bat [output-name]
rem          default: ..\bypass.exe
rem
rem  NOTE: if bypass.exe is running it is locked and the link
rem        will fail. Close it first (tray -> exit, or Ctrl+Alt+Q).
rem
rem  Toolchain: C:\Program Files\RedPanda-Cpp\mingw64\bin
rem  Keep this file ASCII-only.
rem ============================================================
setlocal
set MINGW=C:\Program Files\RedPanda-Cpp\mingw64\bin
set PATH=%MINGW%;%PATH%

set ROOT=%~dp0..
set OUT=%ROOT%\bin\
set SRC=%ROOT%\src\bypass.c

if not exist "%OUT%" mkdir "%OUT%"

set TARGET=bypass.exe
if not "%~1"=="" set TARGET=%~1

if not exist "%SRC%" (
    echo [build] FAILED - source not found: %SRC%
    exit /b 1
)
if not exist "%MINGW%\gcc.exe" (
    echo [build] FAILED - compiler not found: %MINGW%\gcc.exe
    exit /b 1
)

echo [build] compiler : %MINGW%\gcc.exe
"%MINGW%\gcc.exe" --version | findstr /i "gcc.exe"

"%MINGW%\gcc.exe" -mwindows -O2 -D__USE_MINGW_ANSI_STDIO=1 ^
    -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable ^
    "%SRC%" -o "%OUT%%TARGET%" ^
    -lgdi32 -lcomctl32 -ladvapi32 -lshell32 -luser32 -lshlwapi -lwinmm -liphlpapi

if errorlevel 1 (
    echo.
    echo [build] FAILED
    echo [build] If the error says "Permission denied", bypass.exe is still running.
    echo [build] Close it first: tray icon -^> Exit  ^(or Ctrl+Alt+Q^), then run this again.
    exit /b 1
)
echo [build] OK -^> %OUT%%TARGET%
endlocal
