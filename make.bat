@echo off
rem ============================================================
rem  Classroom Bypass Tool - run GNU Make without touching PATH
rem
rem  Usage:
rem     make.bat              -> build all (bypass + hook32)
rem     make.bat bypass       -> only bypass.exe
rem     make.bat hook32       -> only bypass_hook32.dll + inject32.exe
rem     make.bat test         -> bin\hooktest32.exe
rem     make.bat check        -> print both gcc versions
rem     make.bat rebuild      -> clean + all
rem     make.bat clean        -> delete outputs (incl. the 3 deliverables)
rem
rem  Extra arguments go straight to mingw32-make, e.g.:
rem     make.bat -n all
rem     make.bat -B hook32
rem
rem  The two toolchains are prepended to PATH only inside this
rem  script (setlocal). The system PATH is not modified.
rem  Keep this file ASCII-only.
rem ============================================================
setlocal
set "ROOT=%~dp0"
set "MINGW64=C:\Program Files\RedPanda-Cpp\mingw64\bin"
set "MINGW32=C:\mingw32\bin"

if not exist "%MINGW64%\mingw32-make.exe" (
    echo [make] FAILED - make not found: %MINGW64%\mingw32-make.exe
    exit /b 1
)
if not exist "%MINGW64%\gcc.exe" (
    echo [make] FAILED - 64-bit gcc not found: %MINGW64%\gcc.exe
    exit /b 1
)
if not exist "%MINGW32%\i686-w64-mingw32-gcc.exe" (
    echo [make] FAILED - 32-bit gcc not found: %MINGW32%\i686-w64-mingw32-gcc.exe
    exit /b 1
)

set "PATH=%MINGW64%;%MINGW32%;%PATH%"

pushd "%ROOT%"
"%MINGW64%\mingw32-make.exe" %*
set "RC=%ERRORLEVEL%"
popd

if not "%RC%"=="0" (
    echo.
    echo [make] FAILED ^(exit %RC%^)
    echo [make] If it says Permission denied, close bypass.exe / classroom software first.
) else (
    echo [make] OK
)

rem Keep the window open only when double-clicked (cmd /c "...\make.bat", no args).
echo %cmdcmdline% | find /i "%~nx0" >nul
if not errorlevel 1 if "%~1"=="" pause

exit /b %RC%
