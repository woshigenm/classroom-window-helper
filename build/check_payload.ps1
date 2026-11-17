# Guard: do not stage a payload that no longer matches lpe\src.
#
# `make.bat lpe` copies lpe\payload into bin\lpe.  lpepayload is an explicit
# target (rebuilding changes the binaries' SHA256), so without this check a
# source edit plus a plain `make.bat all` silently ships the previous binaries
# and every test stays green.
#
# The hash is taken over LF-normalized bytes: .gitattributes forces eol=lf for
# *.c, and a checkout that produced CRLF must not look like a source change
# (that would demand a payload rebuild for nothing).  CRLF is reported
# separately instead, because it means the working tree is not the form the
# repository stores.
#
# Content hash rather than timestamps: a fresh clone/checkout gives every file
# the same mtime, so an mtime check would either fire on a clean tree or miss a
# same-second edit.
param([switch]$Update)

$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$names  = @('documents_path.c', 'wallet_callback_shell.c', 'wallet_ese_seed.c')
$record = Join-Path $root 'lpe\payload\SOURCES.sha256'

$bytes  = New-Object System.IO.MemoryStream
$crlf   = @()
foreach ($n in $names) {
    $path = Join-Path $root "lpe\src\$n"
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Host "[payload] missing $path"
        exit 2
    }
    $data = [IO.File]::ReadAllBytes($path)
    $norm = New-Object byte[] $data.Length
    $len  = 0
    $cr   = 0
    for ($i = 0; $i -lt $data.Length; $i++) {
        if ($data[$i] -eq 13 -and ($i + 1) -lt $data.Length -and $data[$i + 1] -eq 10) {
            $cr++
            continue
        }
        $norm[$len++] = $data[$i]
    }
    if ($cr -gt 0) { $crlf += "$n ($cr)" }
    $bytes.Write($norm, 0, $len)
}

$sha  = [Security.Cryptography.SHA256]::Create()
$hash = ([BitConverter]::ToString($sha.ComputeHash($bytes.ToArray()))) -replace '-', ''
$sha.Dispose()
$bytes.Dispose()

if ($crlf.Count -gt 0) {
    Write-Host "[payload] warning: CRLF in $($crlf -join ', ') - .gitattributes says eol=lf for *.c"
}

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
