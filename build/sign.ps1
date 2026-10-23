param(
    [Parameter(Mandatory = $true)][string]$Pfx,
    [string]$Password = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com",
    [switch]$NoTimestamp,
    [string]$BinDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $BinDir) {
    $BinDir = Join-Path $PSScriptRoot "..\bin"
}

$Pfx = (Resolve-Path -LiteralPath $Pfx).Path
$BinDir = (Resolve-Path -LiteralPath $BinDir).Path

$files = @("bypass.exe", "bypass_hook32.dll", "inject32.exe") |
    ForEach-Object { Join-Path $BinDir $_ }

foreach ($f in $files) {
    if (-not (Test-Path -LiteralPath $f)) {
        throw "missing file: $f"
    }
}

$secure = if ($Password -ne "") {
    ConvertTo-SecureString $Password -AsPlainText -Force
} else {
    Read-Host -AsSecureString "PFX password"
}
$imported = Import-PfxCertificate -FilePath $Pfx -CertStoreLocation Cert:\CurrentUser\My -Password $secure
$cert = Get-Item ("Cert:\CurrentUser\My\" + $imported.Thumbprint)

try {
    foreach ($f in $files) {
        Write-Output ("signing " + $f)
        $params = @{
            FilePath    = $f
            Certificate = $cert
        }
        if (-not $NoTimestamp -and $TimestampUrl -ne "") {
            $params.TimestampServer = $TimestampUrl
        }
        $sig = Set-AuthenticodeSignature @params
        if (-not $sig.SignerCertificate) {
            throw ("signing failed: " + $f)
        }
        $check = Get-AuthenticodeSignature -FilePath $f
        Write-Output ("  status=" + $check.Status + "  signer=" + $check.SignerCertificate.Subject)
    }
} finally {
    Remove-Item ("Cert:\CurrentUser\My\" + $imported.Thumbprint) -Force -ErrorAction SilentlyContinue
}

Write-Output "done"
