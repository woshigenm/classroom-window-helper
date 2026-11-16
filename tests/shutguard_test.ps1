$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$ini  = Join-Path $bin 'bypass_shut.ini'
$exe  = Join-Path $bin 'shutguard_test.exe'
$dll  = Join-Path $bin 'bypass_hook32.dll'
$log  = Join-Path $bin 'bypass_hook.log'

if (!(Test-Path $exe)) { throw "missing $exe (run: make.bat shuttest)" }
if (!(Test-Path $dll)) { throw "missing $dll (run: make.bat hook32)" }

# Hard safety gate: the test must not be able to ask the OS to power off.
# (A previous revision called ExitWindowsEx(0x80000000) "as a harmless probe";
#  Windows treated it as a shutdown.  Never again.)
$ascii = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($exe))
$wide  = [System.Text.Encoding]::Unicode.GetString([System.IO.File]::ReadAllBytes($exe))
foreach ($bad in @("ExitWindowsEx`0", 'InitiateSystemShutdown')) {
    if ($ascii.Contains($bad)) {
        throw "UNSAFE test binary: imports '$($bad -replace "`0", '')' - refusing to run"
    }
}
# A bare shutdown command would resolve to System32\shutdown.exe.  The test may
# only ever use the fake probe path (built at run time), never this literal.
foreach ($bad in @('shutdown -s', 'shutdown /s', 'shutdown -r', 'shutdown /r',
                   'shutdown -t', 'shutdown /t')) {
    if ($ascii.Contains($bad) -or $wide.Contains($bad)) {
        throw "UNSAFE test binary: literal '$bad' - refusing to run"
    }
}

$bak = $null
if (Test-Path $ini) { $bak = [System.IO.File]::ReadAllText($ini) }

function Write-Ini([int]$v) {
    [System.IO.File]::WriteAllText($ini, "enabled=$v`r`n",
        (New-Object System.Text.UTF8Encoding($false)))
}

$logStart = 0
if (Test-Path $log) { $logStart = (Get-Item $log).Length }

Write-Ini 0
$out1 = & $exe --expect off --inject $dll 2>&1
$rc1  = $LASTEXITCODE

Write-Ini 1
$out2 = & $exe --expect on --inject $dll 2>&1
$rc2  = $LASTEXITCODE

if ($null -ne $bak) {
    [System.IO.File]::WriteAllText($ini, $bak)
} else {
    Remove-Item $ini -Force -ErrorAction SilentlyContinue
}

# Nothing to cancel: the test only ever runs a fake shutdown.exe.


'--- guard off (expect nothing intercepted) ---'
$out1 | ForEach-Object { "$_" }
"rc1=$rc1"

'--- guard on (expect everything swallowed) ---'
$out2 | ForEach-Object { "$_" }
"rc2=$rc2"

'--- new [Guard] lines in bin\bypass_hook.log ---'
if (Test-Path $log) {
    $fs = [System.IO.File]::Open($log, [System.IO.FileMode]::Open,
                                 [System.IO.FileAccess]::Read,
                                 [System.IO.FileShare]::ReadWrite)
    [void]$fs.Seek($logStart, [System.IO.SeekOrigin]::Begin)
    $sr = New-Object System.IO.StreamReader($fs, [System.Text.Encoding]::Default)
    $tail = $sr.ReadToEnd()
    $sr.Close()
    $fs.Close()
    ($tail -split "`r?`n" | Where-Object { $_ -match '\[Guard\]' }) |
        Select-Object -First 20
} else {
    '(no log)'
}

$ok = ($rc1 -eq 0) -and ($rc2 -eq 0) -and
      ($out1 -match 'VERDICT PASS') -and ($out2 -match 'VERDICT PASS')
'VERDICT ' + $(if ($ok) { 'PASS' } else { 'FAIL' })
if (-not $ok) { exit 1 }
