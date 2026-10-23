param(
    [string]$Version = "v1.2.0",
    [string]$OutDir = "",
    [string]$Pfx = "",
    [string]$Password = "",
    [switch]$NoTimestamp,
    [switch]$IncludeCert
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path $Root "dist" }
$Bin = Join-Path $Root "bin"

$required = @("bypass.exe", "bypass_hook32.dll", "inject32.exe")
foreach ($name in $required) {
    $p = Join-Path $Bin $name
    if (-not (Test-Path -LiteralPath $p)) { throw "missing: $p" }
}

if ($Pfx -ne "") {
    $signArgs = @{
        Pfx      = $Pfx
        Password = $Password
    }
    if ($NoTimestamp) { $signArgs.NoTimestamp = $true }
    & (Join-Path $PSScriptRoot "sign.ps1") @signArgs
}

foreach ($name in $required) {
    $p = Join-Path $Bin $name
    $sig = Get-AuthenticodeSignature -FilePath $p
    if ($sig.Status -eq "NotSigned") {
        Write-Warning ("not signed: " + $name)
    } else {
        Write-Output ("{0}: {1}" -f $name, $sig.Status)
    }
}

$stage = Join-Path $env:TEMP ("cwh-pkg-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $stage -Force | Out-Null
$stageBin = Join-Path $stage "bin"
New-Item -ItemType Directory -Path $stageBin -Force | Out-Null

try {
    foreach ($name in $required) {
        Copy-Item -LiteralPath (Join-Path $Bin $name) -Destination $stageBin
    }

    $cfg = Join-Path $Bin "bypass_config.ini"
    if (Test-Path -LiteralPath $cfg) {
        Copy-Item -LiteralPath $cfg -Destination $stageBin
    } else {
        @(
            "# bypass_tool runtime settings"
            "window_mode=1"
            "title_autohide=1"
            "minimize_mode=taskbar"
            "small="
        ) | Set-Content -LiteralPath (Join-Path $stageBin "bypass_config.ini") -Encoding ASCII
    }

    foreach ($rel in @("README.md", "docs\how-to-use.md", "docs\design.md")) {
        $src = Join-Path $Root $rel
        if (Test-Path -LiteralPath $src) {
            Copy-Item -LiteralPath $src -Destination $stage
        }
    }

    if ($IncludeCert) {
        $cer = Join-Path $Root "local\certs\bypass_tool-test.cer"
        if (Test-Path -LiteralPath $cer) {
            Copy-Item -LiteralPath $cer -Destination $stage
            @(
                "Self-signed test certificate"
                ""
                "On the target machine (current user):"
                "  Import-Certificate -FilePath bypass_tool-test.cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher"
                "  Import-Certificate -FilePath bypass_tool-test.cer -CertStoreLocation Cert:\CurrentUser\Root"
                ""
                "Without importing it, Get-AuthenticodeSignature shows UnknownError."
            ) | Set-Content -LiteralPath (Join-Path $stage "TRUST-CERT.txt") -Encoding ASCII
        } else {
            Write-Warning ("cert not found: " + $cer)
        }
    }

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $zip = Join-Path $OutDir ("classroom-window-helper-" + $Version + ".zip")
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
    Write-Output ("zip: " + $zip)
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
