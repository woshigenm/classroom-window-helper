$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.real.dll'
$wl   = Join-Path $bin 'wmode_test.exe'
$log  = Join-Path $bin 'bypass_hook.log'
$beat = Join-Path $env:TEMP 'bypass_fake_core_beat.txt'

$fakes = @(
    @{ dll = 'fake_core_return0.dll'; tag = 'return 0';
       expect = 'Core_Init returned 0'; beat = $false },
    @{ dll = 'fake_core_abi.dll'; tag = 'bad ABI';
       expect = 'bad ABI'; beat = $true },
    @{ dll = 'fake_core_hooks.dll'; tag = 'incomplete hooks';
       expect = 'hook table incomplete'; beat = $true }
)

if (!(Test-Path $core)) { throw "missing $core" }
if (!(Test-Path $wl))   { throw "missing $wl (run: make.bat wmodetest)" }
foreach ($f in $fakes) {
    if (!(Test-Path (Join-Path $bin $f.dll))) {
        throw "missing $($f.dll) (run: make.bat fakecores)"
    }
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

function Run-Wmode {
    $out = & (Join-Path $bin 'wmode_test.exe') --ticks 2 --inject bypass_hook32.dll 2>&1
    $code = $LASTEXITCODE
    return @{ out = ($out -join "`n"); code = $code }
}

$pass = $true
try {
    Copy-Item $core $save -Force

    foreach ($f in $fakes) {
        Copy-Item (Join-Path $bin $f.dll) $core -Force
        Remove-Item $beat -Force -ErrorAction SilentlyContinue
        $from = if (Test-Path $log) { (Get-Item $log).Length } else { 0 }

        Push-Location $bin
        $r = Run-Wmode
        Pop-Location

        $new = Log-New $from
        $hit = $new -match [regex]::Escape($f.expect)
        $notFixed = $r.out -match 'VERDICT NOT FIXED'
        $beatOk = if ($f.beat) { Test-Path $beat } else { -not (Test-Path $beat) }
        $ok = $hit -and $notFixed -and $beatOk

        "--- fake core: $($f.tag) ---"
        "  log  : " + $(if ($hit) { 'ok' } else { "FAIL (missing '$($f.expect)')" })
        "  table: " + $(if ($notFixed) { 'ok (NOT FIXED / passthrough)' } else { "FAIL (wmode: $($r.out -split "`n" | Select-Object -Last 1))" })
        "  alive: " + $(if ($beatOk) { if ($f.beat) { 'ok (heartbeat after Core_Init)' } else { 'ok (no heartbeat expected)' } } else { 'FAIL (Beat file mismatch)' })
        if (-not $ok) { $pass = $false }
    }
} finally {
    if (Test-Path $save) {
        Copy-Item $save $core -Force
        Remove-Item $save -Force -ErrorAction SilentlyContinue
    }
}

# real core must still work after all the swapping
Push-Location $bin
$r = Run-Wmode
Pop-Location
$fixed = $r.out -match 'VERDICT FIXED'
"--- restored real core ---"
"  wmode: " + $(if ($fixed) { 'FIXED (ok)' } else { 'FAIL (still broken)' })
if (-not $fixed) { $pass = $false }

"VERDICT " + $(if ($pass) { 'PASS (all Loader failure paths degrade to passthrough)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
