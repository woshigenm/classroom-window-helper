$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.real.dll'
$host0 = Join-Path $bin 'core_host.exe'
$wl   = Join-Path $bin 'wmode_test.exe'
$cache = Join-Path $bin 'core_cache'
$log  = Join-Path $bin 'bypass_hook.log'
$pidfile = Join-Path $env:TEMP 'bypass_core_host.pid'

foreach ($f in @('fake_core_good.dll','fake_core_return0.dll','fake_core_abi.dll',
                 'fake_core_leak.dll','core_host.exe')) {
    if (!(Test-Path (Join-Path $bin $f))) { throw "missing $f (run: make.bat fakecores corehost)" }
}
if (!(Test-Path $wl)) { throw "missing wmode_test.exe (run: make.bat wmodetest)" }

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

$pass = $true
$hostProc = $null
$procId = 0
try {
    Copy-Item $core $save -Force
    Copy-Item (Join-Path $bin 'fake_core_good.dll') $core -Force
    $from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

    $hostProc = Start-Process -FilePath $host0 -WorkingDirectory $bin -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', 'bypass_hook32.dll', '--seconds', '60')

    for ($i = 0; $i -lt 60 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw "core_host did not write its pidfile" }
    $procId = [int](Get-Content $pidfile -Raw).Trim()
    Start-Sleep -Milliseconds 1500

    $c1 = Join-Path $cache "core.$procId.1.dll"
    "--- initial load ---"
    "  cache .1 : " + $(if (Test-Path $c1) { 'ok' } else { 'FAIL' })
    if (!(Test-Path $c1)) { $pass = $false }
    $st = Send-Ctl $procId 'status'
    "  status   : $st"
    if ($st -notmatch '^ok core=.* seq=1') { $pass = $false }

    "--- reload success (fake_good -> fake_good) ---"
    $r = Send-Ctl $procId 'reload'
    "  resp     : $r"
    if ($r -notmatch '^ok seq=(\d+)') { $pass = $false }
    $c2 = Join-Path $cache "core.$procId.2.dll"
    if (!(Test-Path $c2)) { $pass = $false; "  cache .2 : FAIL" } else { "  cache .2 : ok" }
    if (Test-Path $c1) { $pass = $false; "  old .1   : FAIL (not deleted)" } else { "  old .1   : ok (deleted)" }

    "--- reload failure: Core_Init returns 0 -> rollback ---"
    Copy-Item (Join-Path $bin 'fake_core_return0.dll') $core -Force
    $r = Send-Ctl $procId 'reload'
    "  resp     : $r"
    if ($r -notmatch '^rolledback seq=(\d+)') { $pass = $false }
    Start-Sleep -Milliseconds 300
    $new = Log-New $from
    if ($new -match 'reload failed: Core_Init returned 0') { "  log      : ok (reload failed)" }
    else { "  log      : FAIL (missing reload failure)"; $pass = $false }
    if ($new -match 'rollback ok:') { "  rollback : ok" } else { "  rollback : FAIL"; $pass = $false }

    "--- reload failure: bad ABI (Core initialized, must stay mapped) -> rollback ---"
    Copy-Item (Join-Path $bin 'fake_core_abi.dll') $core -Force
    $r = Send-Ctl $procId 'reload'
    "  resp     : $r"
    if ($r -notmatch '^rolledback seq=(\d+)') { $pass = $false }
    Start-Sleep -Milliseconds 300
    $new = Log-New $from
    if ($new -match 'bad ABI .* leaving it mapped') { "  log      : ok (left mapped)" }
    else { "  log      : FAIL (missing bad-ABI line)"; $pass = $false }

    "--- reload to a leaking Core, then reload again -> still mapped/coreDead ---"
    Copy-Item (Join-Path $bin 'fake_core_leak.dll') $core -Force
    $r = Send-Ctl $procId 'reload'
    "  leak load: $r"
    if ($r -notmatch '^ok seq=') { $pass = $false }
    Copy-Item (Join-Path $bin 'fake_core_good.dll') $core -Force
    $r = Send-Ctl $procId 'reload'
    "  unload   : $r"
    if ($r -notmatch '^err coreDead') { $pass = $false }
    Start-Sleep -Milliseconds 300
    $new = Log-New $from
    if ($new -match 'still mapped after FreeLibrary') { "  log      : ok (still mapped)" }
    else { "  log      : FAIL (missing still-mapped line)"; $pass = $false }

    $st = Send-Ctl $procId 'status'
    "  status   : $st"
    if ($st -notmatch 'dead=1') { $pass = $false }
    $r = Send-Ctl $procId 'reload'
    "  again    : $r"
    if ($r -notmatch '^err coreDead') { $pass = $false }

    $hp = Get-Process -Id $procId -ErrorAction SilentlyContinue
    "  host     : " + $(if ($hp) { 'alive (ok)' } else { 'FAIL (crashed)' })
    if (!$hp) { $pass = $false }
} finally {
    if ($hostProc -and !$hostProc.HasExited) { Stop-Process -Id $hostProc.Id -Force -ErrorAction SilentlyContinue }
    if ($procId -gt 0) {
        $hp = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if ($hp) { Stop-Process -Id $hp.Id -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path $save) {
        Copy-Item $save $core -Force
        Remove-Item $save -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
}

# real core must still work after all the swapping
Push-Location $bin
$out = & $wl --ticks 2 --inject bypass_hook32.dll 2>&1
Pop-Location
$fixed = ($out -join "`n") -match 'VERDICT FIXED'
"--- restored real core ---"
"  wmode    : " + $(if ($fixed) { 'FIXED (ok)' } else { 'FAIL' })
if (-not $fixed) { $pass = $false }

# P2b-1: the real Core now has before_unload (threads) -> reload must succeed
$host2 = $null
$pid2 = 0
try {
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    $host2 = Start-Process -FilePath $host0 -WorkingDirectory $bin -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', 'bypass_hook32.dll', '--seconds', '20')
    for ($i = 0; $i -lt 60 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw "core_host (real) did not write its pidfile" }
    $pid2 = [int](Get-Content $pidfile -Raw).Trim()
    Start-Sleep -Milliseconds 1500
    $from2 = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    $r = Send-Ctl $pid2 'reload'
    "--- real Core reload (P2b-1: must succeed) ---"
    "  resp     : $r"
    if ($r -notmatch '^ok seq=') { $pass = $false }
    Start-Sleep -Milliseconds 500
    $new2 = Log-New $from2
    if ($new2 -match '\[Core\] before_unload: worker \+ caption \+ vd stopped') { "  log      : ok (threads stopped)" }
    else { "  log      : FAIL (missing before_unload line)"; $pass = $false }
    if ($new2 -match '\[Loader\] core loaded:') { "  new core : ok" }
    else { "  new core : FAIL"; $pass = $false }
    $st = Send-Ctl $pid2 'status'
    "  status   : $st"
} finally {
    if ($host2 -and !$host2.HasExited) { Stop-Process -Id $host2.Id -Force -ErrorAction SilentlyContinue }
    if ($pid2 -gt 0) {
        $hp = Get-Process -Id $pid2 -ErrorAction SilentlyContinue
        if ($hp) { Stop-Process -Id $hp.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (cache/reload/rollback/still-mapped)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
