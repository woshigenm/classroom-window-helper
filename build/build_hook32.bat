@echo off
rem ============================================================
rem  Classroom Bypass Tool - build the 32-bit components
rem    bypass_hook32.dll  <- ..\src\bypass_hook.c
rem    inject32.exe       <- ..\src\inject32.c
rem  Output: ..\  (must stay next to bypass.exe)
rem
rem  Toolchain: C:\mingw32  (i686-w64-mingw32-gcc)
rem
rem  -static -static-libgcc is REQUIRED for the DLL: without it
rem  LoadLibraryW() inside the target process returns 0.
rem
rem  NOTE: keep this file ASCII-only. No parentheses inside
rem        parenthesized IF blocks - cmd parses them as syntax.
rem ============================================================
setlocal
set BIN=C:\mingw32\bin
set PATH=%BIN%;%PATH%

set ROOT=%~dp0..
set OUT=%ROOT%\bin\
set SRC=%ROOT%\src

if not exist "%OUT%" mkdir "%OUT%"

if not exist "%BIN%\i686-w64-mingw32-gcc.exe" (
    echo [FAILED] 32-bit toolchain not found at %BIN%
    exit /b 1
)

echo [1/2] bypass_hook32.dll
if exist "%OUT%bypass_hook32.old.dll" del /f /q "%OUT%bypass_hook32.old.dll" >nul 2>nul
"%BIN%\i686-w64-mingw32-gcc.exe" -std=gnu11 -O2 -Wall -shared "%SRC%\bypass_hook.c" -o "%OUT%bypass_hook32.dll" -lpsapi -Wl,--kill-at -static -static-libgcc
if errorlevel 1 (
    echo [FAILED] bypass_hook32.dll
    echo [FAILED] If it says Permission denied, the classroom software still has the DLL loaded.
    echo [FAILED] Close Student / MultiClient / ScreenRender, then run this script again.
    exit /b 1
)
echo       OK

echo [2/2] inject32.exe
"%BIN%\i686-w64-mingw32-gcc.exe" -std=gnu11 -O2 -Wall "%SRC%\inject32.c" -o "%OUT%inject32.exe"
if errorlevel 1 (
    echo [FAILED] inject32.exe
    exit /b 1
)
echo       OK

echo.
dir "%OUT%bypass_hook32.dll" "%OUT%inject32.exe"
endlocal
