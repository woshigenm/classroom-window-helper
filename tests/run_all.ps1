# run_all.ps1 -- build every test artifact and run every regression suite.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_all.ps1
#   ... -NoBuild        skip "make.bat" (use the DLLs already in bin\)
#   ... -Filter reload  only suites whose name matches the pattern
#
# Exit code 0 only if every suite passed.  Suites that need the real classroom
# client are not here; these all run against core_host.exe / the C harnesses.

[CmdletBinding()]
param(
    [switch]$NoBuild,
    [string]$Filter = '*'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$tests = $PSScriptRoot

function Invoke-Build([string[]]$targets) {
    Push-Location $root
    try {
        foreach ($t in $targets) {
            Write-Host "[build] make.bat $t" -ForegroundColor Cyan
            & cmd /c "make.bat $t" 2>&1 | ForEach-Object { if ($_ -match 'warning:|error:') { Write-Host "  $_" -ForegroundColor Yellow } }
            if ($LASTEXITCODE -ne 0) { throw "make.bat $t failed ($LASTEXITCODE)" }
        }
    } finally { Pop-Location }
}

function Run-Suite([string]$name, [scriptblock]$body) {
    if ($name -notlike $Filter) { return }
    Write-Host ""
    Write-Host "=== $name ===" -ForegroundColor Cyan
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $script:lastStatus = $null
    try {
        & $body
        $code = $LASTEXITCODE
        if ($null -eq $code) { $code = 0 }
    } catch {
        Write-Host "  EXCEPTION: $($_.Exception.Message)" -ForegroundColor Red
        $code = 1
    }
    $sw.Stop()
    $ok = ($code -eq 0)
    $script:results += [pscustomobject]@{
        Suite  = $name
        Result = if ($ok) { 'PASS' } else { 'FAIL' }
        Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    }
    if (-not $ok) { $script:failed++ }
}

# build:
#   make.bat all / fakecores / testcores / corehost / uitest

if (-not $NoBuild) {
    Invoke-Build @('all', 'fakecores', 'testcores', 'corehost', 'uitest',
                   'wmodetest', 'screentest', 'shuttest')
}

$script:results = @()
$script:failed  = 0

Run-Suite 'fake cores (Loader failure paths)' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'fake_core_test.ps1') |
        Out-Host
}

Run-Suite 'reload / rollback / still-mapped / uef' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'reload_test.ps1') |
        Out-Host
}

Run-Suite 'stuck hook / worker / subclass' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'stuck_test.ps1') |
        Out-Host
}

# Terminal suite: coreDead is process-global, so this one is last among the
# reload-family tests and must not be followed by "recover and reload" checks.
Run-Suite 'malicious Core (drain timeout -> coreDead)' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'evil_core_test.ps1') |
        Out-Host
}

Run-Suite 'window mode (topmost stripped)' {
    Push-Location $bin
    try { & ".\wmode_test.exe" --ticks 2 --inject bypass_hook32.dll | Out-Host }
    finally { Pop-Location }
}

Run-Suite 'screen guard (frozen frame)' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'screenguard_test.ps1') |
        Out-Host
}

Run-Suite 'foreign fix stayed async' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'foreign_fix_test.ps1') |
        Out-Host
}

Run-Suite 'shutdown guard' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'shutguard_test.ps1') |
        Out-Host
}

# Re-entrancy / disabled-Core semantics: after a failed phase 2 the 6 inline
# hooks that used to skip the g_coreDisabled check must pass through.
Run-Suite 'reentry (disabled Core passthrough)' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'reentry_test.ps1') |
        Out-Host
}

Run-Suite 'version self-check (content hash)' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'version_test.ps1') |
        Out-Host
}

# Multi-instance / concurrency: four classroom processes, one UI click, two
# clients on the same pipe, a status check during a slow reload, crash
# isolation.  Runs last: it swaps the Core DLL around (always restored).
Run-Suite 'multi-instance / pipe concurrency' {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tests 'multi_test.ps1') |
        Out-Host
}

Write-Host ""
Write-Host "================ SUMMARY ================" -ForegroundColor Cyan
$script:results | Format-Table -AutoSize | Out-Host

if ($script:failed -gt 0) {
    Write-Host "$($script:failed) suite(s) FAILED" -ForegroundColor Red
    exit 1
}
Write-Host "all suites PASS" -ForegroundColor Green
