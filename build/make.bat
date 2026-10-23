@echo off
rem ============================================================
rem  Forward to ..\make.bat (adds the toolchains to PATH).
rem  Works from build\ just like the old build_*.bat scripts.
rem  Keep this file ASCII-only.
rem ============================================================
call "%~dp0..\make.bat" %*
exit /b %ERRORLEVEL%
