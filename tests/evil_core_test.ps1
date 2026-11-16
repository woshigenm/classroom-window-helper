$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.real.dll'
$host0 = Join-Path $bin 'core_host.exe'
$log  = Join-Path $bin 'bypass_hook.log'
$pidfile = Join-Path $env:TEMP 'bypass_evil_host.pid'

if (!(Test-Path (Join-Path $bin 'core_evil.dll'))) {
    throw "missing core_evil.dll (run: make.bat testcores)"
}

function Send-Ctl([int]$procId, [string]$msg) {
    $name = "bypass_core_ctl_$procId"
    $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name,
                [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $pipe.Connect(8000)
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
$p = $null
$hostId = 0
try {
    Copy-Item $core $save -Force
    Copy-Item (Join-Path $bin 'core_evil.dll') $core -Force
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    $p = Start-Process -FilePath $host0 -WorkingDirectory $bin -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', 'bypass_hook32.dll',
                        '--window', '--seconds', '30')
    for ($i = 0; $i -lt 150 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw "evil host did not write its pidfile" }
    $hostId = [int](Get-Content $pidfile -Raw).Trim()
    Start-Sleep -Milliseconds 2500          # evil thread enters ShowWindow (60 s)

    $from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }
    "--- malicious Core (drain timeout) ---"
    $r = Send-Ctl $hostId 'reload'          # blocks ~3 s inside the Loader drain
    "  reload   : $r"
    if ($r -notmatch '^err hooks-busy \(core disabled\)') { $pass = $false }
    Start-Sleep -Milliseconds 300
    $lines = Log-New $from
    if ($lines -match 'reload aborted: .*hooks still executing; core disabled') {
        "  log      : ok (drain aborted, core disabled)"
    } else { "  log      : FAIL"; $pass = $false }

    $st = Send-Ctl $hostId 'status'
    "  status   : $st"
    if ($st -notmatch 'dead=1') { $pass = $false }

    $r = Send-Ctl $hostId 'reload'
    "  again    : $r"
    if ($r -notmatch '^err coreDead') { $pass = $false }

    if (Get-Process -Id $hostId -ErrorAction SilentlyContinue) { "  host     : alive (ok)" }
    else { "  host     : FAIL (crashed)"; $pass = $false }
} finally {
    if ($p -and !$p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue }
    if ($hostId -gt 0) {
        $hp = Get-Process -Id $hostId -ErrorAction SilentlyContinue
        if ($hp) { Stop-Process -Id $hp.Id -Force -ErrorAction SilentlyContinue }
    }
    if (Test-Path $save) {
        Copy-Item $save $core -Force
        Remove-Item $save -Force -ErrorAction SilentlyContinue
    }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (malicious Core: drain timeout -> coreDead)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
