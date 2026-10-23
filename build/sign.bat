@echo off
rem Usage: build\sign.bat [pfx-path] [notimestamp]
rem If pfx-path is omitted, it prompts. Password is prompted by PowerShell.
setlocal
cd /d "%~dp0.."

set "PFX=%~1"
if "%PFX%"=="" set /p PFX=PFX path: 
if not exist "%PFX%" (
    echo [sign] PFX not found: %PFX%
    exit /b 1
)

set "EXTRA="
if /i "%~2"=="notimestamp" set "EXTRA=-NoTimestamp"

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sign.ps1" -Pfx "%PFX%" %EXTRA%
if errorlevel 1 (
    echo [sign] FAILED
    exit /b 1
)
echo [sign] OK
exit /b 0
