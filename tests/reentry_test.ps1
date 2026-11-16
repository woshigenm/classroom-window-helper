# reentry_test.ps1 -- the 6 inline hooks that used to skip the g_coreDisabled
# check must pass through once the Core is disabled, so no hook logic runs
# against a half-closed Core (StateClose already released the shared state).
#
# Probe: the host calls WinExec("shutdown /?") twice (1.5 s and 7 s).  The
# shutdown guard is ON, so while the Core is healthy the Core hooks WinExec,
# logs "[Guard] blocked WinExec" and never starts the process.  A reload with a
# worker that never stops fails in phase 2 (g_coreDisabled=1, g_unloading stays
# 0), and from then on the probe must go straight to the real WinExec: no new
# "[Guard] blocked" line.  shutdown /? only prints usage, it never shuts down.
#
# Run from the repo root after `make.bat all testcores corehost`.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$log  = Join-Path $bin 'bypass_hook.log'
$dll  = Join-Path $bin 'bypass_hook32.dll'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.reentry.dll'
$host0 = $null
$pass = $true

function Send-Ctl([int]$procId, [string]$msg, [int]$timeoutMs = 20000) {
    $name = "bypass_core_ctl_$procId"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        try {
            $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name,
                        [System.IO.Pipes.PipeDirection]::InOut)
            $pipe.Connect(200)
            break
        } catch {
            if ($sw.ElapsedMilliseconds -gt $timeoutMs) { return $null }
            Start-Sleep -Milliseconds 50
        }
    }
    try {
        $b = [System.Text.Encoding]::ASCII.GetBytes($msg)
        $pipe.Write($b, 0, $b.Length); $pipe.Flush()
        $buf = New-Object byte[] 1024
        $n = $pipe.Read($buf, 0, $buf.Length)
        if ($n -le 0) { return '' }
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n).Trim()
    } finally { $pipe.Dispose() }
}

function Count-Blocked([int]$procId) {
    if (!(Test-Path $log)) { return 0 }
    @(Get-Content $log |
      Where-Object { $_ -match '\[Guard\] blocked WinExec' -and $_ -match "\[pid $procId\]" }).Count
}

$pidfile = Join-Path $env:TEMP 'bypass_reentry.pid'
$outfile = Join-Path $env:TEMP 'bypass_reentry.out'
Copy-Item $core $save -Force
try {
    Copy-Item (Join-Path $bin 'core_stuck_worker.dll') $core -Force
    Remove-Item $pidfile, $outfile -Force -ErrorAction SilentlyContinue
    $host0 = Start-Process -FilePath (Join-Path $bin 'core_host.exe') -WorkingDirectory $bin `
        -RedirectStandardOutput $outfile -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', $dll, '--seconds', '14',
                        '--probe-winexec', '1500,7000')
    for ($i = 0; $i -lt 80 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw 'host did not start' }
    $procId = [int](Get-Content $pidfile -Raw).Trim()

    Start-Sleep -Milliseconds 2600            # probe #1 (1.5 s) has run
    $before = Count-Blocked $procId
    "  probe #1 (guard on)   : blocked lines=$before"
    if ($before -lt 1) { '  FAIL: the guard did not intercept the first probe'; $pass = $false }

    $r = Send-Ctl $procId 'reload'            # fails phase 2 after ~3 s
    "  reload               : $r"
    if ($r -notmatch 'refused-by-core|hooks-busy') { '  FAIL: reload did not fail in phase 2'; $pass = $false }
    if ($r -match 'coreDead|core disabled') { '  FAIL: phase-2 failure must not be coreDead'; $pass = $false }

    Start-Sleep -Milliseconds 5000            # probe #2 (7 s) is now in the past
    $after = Count-Blocked $procId
    "  probe #2 (disabled)   : blocked lines=$after"
    if ($after -ne $before) {
        '  FAIL: WinExec still ran the shutdown-guard logic while the Core was disabled'
        $pass = $false
    }

    $probe = @(Get-Content $outfile | Where-Object { $_ -match 'probe winexec' })
    "  host probe output:"
    $probe | ForEach-Object { "    $_" }
    if ($probe.Count -lt 2) { '  FAIL: the second probe never ran'; $pass = $false }
    if (-not ($probe -match 't=1500 ret=33')) { '  FAIL: the guard did not block the first probe (expected ret=33)'; $pass = $false }
    if (-not ($probe -match 't=7000 ret=([0-9]|[12][0-9]|3[01])\b')) { '  FAIL: the disabled Core did not pass through (real WinExec fails with < 32)'; $pass = $false }

    $st = Send-Ctl $procId 'status'
    "  status               : $st"
    if ($st -notmatch 'dead=0') { '  FAIL: process should still be alive and only半禁用'; $pass = $false }
    if (!(Get-Process -Id $procId -ErrorAction SilentlyContinue)) { '  FAIL: host crashed'; $pass = $false }
} finally {
    if ($host0 -and !$host0.HasExited) { Stop-Process -Id $host0.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $pidfile, $outfile -Force -ErrorAction SilentlyContinue
    Copy-Item $save $core -Force
    Remove-Item $save -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (disabled Core: inline hooks passthrough, no half-closed logic)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
