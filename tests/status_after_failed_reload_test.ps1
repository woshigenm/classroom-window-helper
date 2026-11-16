# status_after_failed_reload_test.ps1 -- P0: no UAF in `status` after the old
# Core was unloaded and the new one failed to load.
#
# DoReload frees the old Core, then tries to load the new one.  When the load
# fails and there is no rollback (or the rollback also fails), g_table is NULL
# but g_core still held build_id / hooks.* pointing into the UNLOADED module.
# `status` reads g_core.build_id directly (it does not go through Tbl()), so it
# dereferenced a dangling pointer.  Fix: memset(&g_core, 0) right after
# FreeLibrary, so status answers `core=?`.
#
# Trigger: swap in fake_core_hooks.dll (Core_Init succeeds, hook table is
# incomplete) and send `reload norollback`.  The Loader unloads the real Core,
# fails to publish the fake one, and must still answer `status` without
# crashing.
#
# Run from the repo root after `make.bat all fakecores corehost`.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$dll  = Join-Path $bin 'bypass_hook32.dll'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.uaf.bak'
$pidfile = Join-Path $env:TEMP 'bypass_uaf.pid'
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

Copy-Item $core $save -Force
try {
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    $host0 = Start-Process -FilePath (Join-Path $bin 'core_host.exe') -WorkingDirectory $bin -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', $dll, '--seconds', '30')
    for ($i = 0; $i -lt 150 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw 'host did not start' }
    $procId = [int](Get-Content $pidfile -Raw).Trim()
    Start-Sleep -Milliseconds 800

    $st = Send-Ctl $procId 'status'
    "  status (healthy)        : $st"
    if ($st -notmatch 'seq=1 dead=0') { '  FAIL: host not ready'; $pass = $false }

    # the fake core: Core_Init returns 1, HooksComplete fails -> LoadCoreFrom
    # returns FALSE and *leaves it mapped*, after the real core was freed
    Copy-Item (Join-Path $bin 'fake_core_hooks.dll') $core -Force
    $r = Send-Ctl $procId 'reload norollback'
    "  reload norollback       : $r"
    if ($r -notmatch 'err reload-failed') { '  FAIL: expected a load failure'; $pass = $false }

    # the UAF: pre-fix this read g_core.build_id from the freed module
    $st2 = Send-Ctl $procId 'status'
    "  status (after failure)  : $st2"
    if (-not $st2) { '  FAIL: no answer (Loader crashed?)'; $pass = $false }
    elseif ($st2 -notmatch '^ok core=\? seq=\d+ dead=0 hash=00000000 cache=-') {
        '  FAIL: status did not report core=? (stale g_core.build_id?)'
        $pass = $false
    }

    if (!(Get-Process -Id $procId -ErrorAction SilentlyContinue)) { '  FAIL: host died'; $pass = $false }
    else { "  host                    : alive (ok)" }
} finally {
    if ($host0 -and !$host0.HasExited) { Stop-Process -Id $host0.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    Copy-Item $save $core -Force
    Remove-Item $save -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (status after failed reload: core=?, no UAF)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
