# reload_hooks_test.ps1 -- hooks must be LIVE again after a successful reload.
#
# Regression for: g_unloading was set to 1 before the drain and never reset on
# the success path, so every dispatcher / inline hook stayed in passthrough
# after a reload (only the Core's Worker kept working).  On the real client that
# looked like: SwitchDesktop no longer blocked (desktop switched to
# NetTeachingDesktop), lpDesktop no longer cleared, no forced window mode and
# SetWindowsHookEx no longer suppressed.
#
# Probe: the host installs a WH_MOUSE_LL hook at 900 ms (before the reload) and
# at 3500 ms (after it).  While the Core is live the hook is suppressed and the
# call returns (HHOOK)0x1 with a "suppressed SetWindowsHookExW(id=14)" line; if
# the dispatcher is stuck in passthrough a real handle comes back and no line is
# written.  The probe unhooks immediately and its proc only calls
# CallNextHookEx, so the mouse is never affected.
#
# Run from the repo root after `make.bat all corehost`.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$log  = Join-Path $bin 'bypass_hook.log'
$dll  = Join-Path $bin 'bypass_hook32.dll'
$pidfile = Join-Path $env:TEMP 'bypass_reload_hooks.pid'
$outfile = Join-Path $env:TEMP 'bypass_reload_hooks.out'
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

function Count-Suppressed([int]$procId) {
    if (!(Test-Path $log)) { return 0 }
    @(Get-Content $log |
      Where-Object { $_ -match 'suppressed SetWindowsHookExW\(id=14\)' -and $_ -match "\[pid $procId\]" }).Count
}

try {
    Remove-Item $pidfile, $outfile -Force -ErrorAction SilentlyContinue
    $host0 = Start-Process -FilePath (Join-Path $bin 'core_host.exe') -WorkingDirectory $bin `
        -RedirectStandardOutput $outfile -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', $dll, '--seconds', '12',
                        '--probe-llhook', '900,3500')
    for ($i = 0; $i -lt 150 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw 'host did not start' }
    $procId = [int](Get-Content $pidfile -Raw).Trim()

    Start-Sleep -Milliseconds 1600                 # probe #1 (900 ms) is done
    $before = Count-Suppressed $procId
    "  probe #1 (before reload) : suppressed lines=$before"
    if ($before -lt 1) { '  FAIL: the Core did not suppress the first probe'; $pass = $false }

    $r = Send-Ctl $procId 'reload'
    "  reload                   : $r"
    if ($r -notmatch '^ok seq=2') { '  FAIL: reload failed'; $pass = $false }

    Start-Sleep -Milliseconds 3000                 # probe #2 (3500 ms) is done
    $after = Count-Suppressed $procId
    "  probe #2 (after reload)  : suppressed lines=$after"
    if ($after -le $before) {
        '  FAIL: the WH_MOUSE_LL hook was NOT suppressed after the reload'
        '        (dispatchers still in drain passthrough: g_unloading not reset)'
        $pass = $false
    }

    $probe = @(Get-Content $outfile | Where-Object { $_ -match 'probe llhook' })
    "  host probe output:"
    $probe | ForEach-Object { "    $_" }
    $ok = @($probe | Where-Object { $_ -match 'ret=0+1\b' })
    if ($ok.Count -lt 2) { '  FAIL: a probe returned a real hook handle (not suppressed)'; $pass = $false }

    $st = Send-Ctl $procId 'status'
    "  status                   : $st"
    if ($st -notmatch 'seq=2 dead=0') { '  FAIL: status'; $pass = $false }
    if (!(Get-Process -Id $procId -ErrorAction SilentlyContinue)) { '  FAIL: host crashed'; $pass = $false }
} finally {
    if ($host0 -and !$host0.HasExited) { Stop-Process -Id $host0.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $pidfile, $outfile -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (hooks live after reload: SetWindowsHookEx suppressed again)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
