# Guard: do not stage a payload that no longer matches lpe\src.
#
# `make.bat lpe` copies lpe\payload into bin\lpe.  lpepayload is an explicit
# target (rebuilding changes the binaries' SHA256), so without this check a
# source edit plus a plain `make.bat all` silently ships the previous binaries
# and every test stays green.
#
# Content hash, not timestamps: a fresh clone/checkout gives every file the
# same mtime, so an mtime check would either fire on a clean tree or miss a
# same-second edit.
param([switch]$Update)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$names  = @('documents_path.c', 'wallet_callback_shell.c', 'wallet_ese_seed.c')
$record = Join-Path $root 'lpe\payload\SOURCES.sha256'

$bytes = New-Object System.IO.MemoryStream
foreach ($n in $names) {
    $path = Join-Path $root "lpe\src\$n"
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "[payload] missing $path"
        exit 2
    }
    $data = [IO.File]::ReadAllBytes($path)
    $bytes.Write($data, 0, $data.Length)
}
$sha  = [Security.Cryptography.SHA256]::Create()
$hash = ([BitConverter]::ToString($sha.ComputeHash($bytes.ToArray()))) -replace '-', ''
$sha.Dispose()
$bytes.Dispose()

if ($Update) {
    Set-Content -LiteralPath $record -Value $hash -Encoding ASCII
    Write-Host "[payload] sources hash recorded: $hash"
    exit 0
}

if (-not (Test-Path -LiteralPath $record)) {
    Write-Host '[payload] lpe\payload\SOURCES.sha256 is missing'
    Write-Host '[payload] run "make.bat lpepayload", then "make.bat lpe"'
    exit 1
}
$want = (Get-Content -LiteralPath $record -Raw).Trim()
if ($want -ne $hash) {
    Write-Host '[payload] lpe\src has changed since lpe\payload was built'
    Write-Host "[payload]   recorded: $want"
    Write-Host "[payload]   now:      $hash"
    Write-Host '[payload] run "make.bat lpepayload", then "make.bat lpe"'
    exit 1
}
exit 0
