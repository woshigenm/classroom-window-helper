@echo off
rem ============================================================
rem  Classroom Bypass Tool - run GNU Make without touching PATH
rem
rem  Usage:
rem     make.bat              -> build all (bypass + hook32 + lpe)
rem     make.bat bypass       -> only bypass.exe
rem     make.bat hook32       -> only bypass_hook32.dll + inject32.exe
rem     make.bat lpe          -> only bin\lpe\ (auto-elevation payload)
rem     make.bat test         -> bin\hooktest32.exe
rem     make.bat wmodetest    -> bin\wmode_test.exe
rem     make.bat vdtest       -> bin\vd_pin_test.exe
rem     make.bat hangwintest  -> bin\hangwin_test.exe (foreign-fix test victim)
rem     make.bat screentest   -> bin\screenguard_test.exe (screen-guard test)
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

rem --- 32-bit toolchain (required): RedPanda-Cpp ships an i686 MinGW ---
set "MINGW32="
if exist "C:\Program Files (x86)\RedPanda-Cpp\mingw32\bin\i686-w64-mingw32-gcc.exe" set "MINGW32=C:\Program Files (x86)\RedPanda-Cpp\mingw32\bin"
if not defined MINGW32 if exist "C:\Program Files\RedPanda-Cpp\mingw32\bin\i686-w64-mingw32-gcc.exe" set "MINGW32=C:\Program Files\RedPanda-Cpp\mingw32\bin"
if not defined MINGW32 if exist "C:\mingw32\bin\i686-w64-mingw32-gcc.exe" set "MINGW32=C:\mingw32\bin"

rem --- 64-bit toolchain (optional): not every install has one ---
set "MINGW64="
if exist "C:\Program Files\RedPanda-Cpp\mingw64\bin\x86_64-w64-mingw32-gcc.exe" set "MINGW64=C:\Program Files\RedPanda-Cpp\mingw64\bin"
if not defined MINGW64 if exist "C:\Program Files (x86)\RedPanda-Cpp\mingw64\bin\x86_64-w64-mingw32-gcc.exe" set "MINGW64=C:\Program Files (x86)\RedPanda-Cpp\mingw64\bin"

if not defined MINGW32 (
    echo [make] FAILED - 32-bit gcc not found for any known location
    exit /b 1
)
if not defined MINGW64 (
    set "MINGW64=%MINGW32%"
    echo [make] note: no x86_64 toolchain found - bypass.exe is built 32-bit
)

set "MAKE=%MINGW32%\mingw32-make.exe"
if not exist "%MAKE%" set "MAKE=%MINGW64%\mingw32-make.exe"
if not exist "%MAKE%" (
    echo [make] FAILED - make not found next to the toolchain
    exit /b 1
)

set "PATH=%MINGW64%;%MINGW32%;%PATH%"

pushd "%ROOT%"
"%MAKE%" CC64="%MINGW64%\gcc.exe" CC32="%MINGW32%\i686-w64-mingw32-gcc.exe" %*
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
