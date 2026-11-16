$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.real.dll'
$host0 = Join-Path $bin 'core_host.exe'
$log  = Join-Path $bin 'bypass_hook.log'
$pidfile = Join-Path $env:TEMP 'bypass_stuck_host.pid'

foreach ($f in @('core_stuck_hook.dll','core_stuck_worker.dll','core_subclass_fail.dll','core_host.exe')) {
    if (!(Test-Path (Join-Path $bin $f))) { throw "missing $f (run: make.bat testcores corehost)" }
}

function Send-Ctl([int]$procId, [string]$msg) {
    $name = "bypass_core_ctl_$procId"
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name,
                [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(4000)
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $buf = New-Object byte[] 1024
        $n = $pipe.Read($buf, 0, $buf.Length)
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n).Trim()
    } finally { $pipe.Dispose() }
}

function Log-New([long]$from) {
    if (!(Test-Path $log)) { return '' }
    $fs = [System.IO.File]::Open($log, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read,
                                 [System.IO.FileShare]::ReadWrite)
    try {
        if ($fs.Length -lt $from) { $from = 0 }
        [void]$fs.Seek($from, [System.IO.SeekOrigin]::Begin)
        $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::Default)
        $t = $sr.ReadToEnd()
        $sr.Close()
        return $t
    } finally { $fs.Close() }
}

function Start-Host([string]$variant, [string]$extraArgs) {
    Copy-Item (Join-Path $bin $variant) $core -Force
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    $args = @('--pidfile', $pidfile, '--inject', 'bypass_hook32.dll', '--seconds', '30')
    if ($extraArgs) { $args += $extraArgs.Split(' ') }
    $p = Start-Process -FilePath $host0 -WorkingDirectory $bin -PassThru -ArgumentList $args
    for ($i = 0; $i -lt 150 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw "host did not write its pidfile" }
    return @{ proc = $p; pid = [int](Get-Content $pidfile -Raw).Trim() }
}

function Stop-Host($h) {
    if ($h.proc -and !$h.proc.HasExited) { Stop-Process -Id $h.proc.Id -Force -ErrorAction SilentlyContinue }
    $hp = Get-Process -Id $h.pid -ErrorAction SilentlyContinue
    if ($hp) { Stop-Process -Id $hp.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
}

$pass = $true
try {
    Copy-Item $core $save -Force

    # ---- 1) hook stuck: phase 1 refuses, then a later reload succeeds ----
    $h = Start-Host 'core_stuck_hook.dll' '--window --hammer-show'
    Start-Sleep -Milliseconds 1500
    $from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $h.pid 'reload'
    Start-Sleep -Milliseconds 300
    $lines = Log-New $from
    "--- hook stuck (phase 1) ---"
    "  reload   : $r"
    if ($r -notmatch '^err refused-by-core') { $pass = $false }
    if ($lines -match 'unload refused: .*hooks still executing') { "  log      : ok (hooks busy)" }
    else { "  log      : FAIL"; $pass = $false }

    Start-Sleep -Milliseconds 6000          # let the first stuck call return
    $from2 = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $h.pid 'reload'
    "  retry    : $r"
    if ($r -notmatch '^ok seq=') { $pass = $false }
    Start-Sleep -Milliseconds 500
    $lines2 = Log-New $from2
    if ($lines2 -match '\[Loader\] core loaded:') { "  new core : ok" }
    else { "  new core : FAIL"; $pass = $false }
    Stop-Host $h

    # ---- 2) worker stuck: phase 2 timeout, then phase 1 refuses ----
    $h = Start-Host 'core_stuck_worker.dll' '--window'
    Start-Sleep -Milliseconds 1500
    $from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $h.pid 'reload'          # waits 3 s inside before_unload
    Start-Sleep -Milliseconds 300
    $lines = Log-New $from
    "--- worker stuck (phase 2) ---"
    "  reload   : $r"
    if ($r -notmatch '^err refused-by-core') { $pass = $false }
    if ($lines -match 'unload failed \(phase 2\): worker did not stop') { "  log      : ok (phase 2 timeout)" }
    else { "  log      : FAIL"; $pass = $false }

    $from2 = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $h.pid 'reload'
    "  again    : $r"
    if ($r -notmatch '^err refused-by-core') { $pass = $false }
    Start-Sleep -Milliseconds 300
    $lines2 = Log-New $from2
    if ($lines2 -match 'unload refused: worker already stopping') { "  phase 1  : ok (refused)" }
    else { "  phase 1  : FAIL"; $pass = $false }
    if (Get-Process -Id $h.pid -ErrorAction SilentlyContinue) { "  host     : alive (ok)" }
    else { "  host     : FAIL (crashed)"; $pass = $false }
    Stop-Host $h

    # ---- 3) subclass remove failed: phase 2 twice, host stays alive ----
    $h = Start-Host 'core_subclass_fail.dll' '--window'
    Start-Sleep -Milliseconds 2000         # let the enforcer subclass the window
    $from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $h.pid 'reload'
    Start-Sleep -Milliseconds 300
    $lines = Log-New $from
    "--- subclass remove failed (phase 2) ---"
    "  reload   : $r"
    if ($r -notmatch '^err refused-by-core') { $pass = $false }
    if ($lines -match 'unload failed \(phase 2\): a window still points') { "  log      : ok (phase 2)" }
    else { "  log      : FAIL"; $pass = $false }

    $from2 = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $h.pid 'reload'
    "  again    : $r"
    if ($r -notmatch '^err refused-by-core') { $pass = $false }
    Start-Sleep -Milliseconds 300
    $lines2 = Log-New $from2
    if ($lines2 -match 'unload failed \(phase 2\): a window still points') { "  phase 2  : ok (refused again)" }
    else { "  phase 2  : FAIL"; $pass = $false }
    if (Get-Process -Id $h.pid -ErrorAction SilentlyContinue) { "  host     : alive (ok)" }
    else { "  host     : FAIL (crashed)"; $pass = $false }
    Stop-Host $h
} finally {
    if (Test-Path $save) {
        Copy-Item $save $core -Force
        Remove-Item $save -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (hook/worker/subclass unload failures)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
