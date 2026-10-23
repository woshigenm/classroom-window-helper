param(
    [string]$Subject = "CN=bypass_tool local test",
    [string]$OutDir = "",
    [string]$Password = "bypass-local-test",
    [switch]$Trust
)

$ErrorActionPreference = "Stop"

if (-not $OutDir) {
    $OutDir = Join-Path $PSScriptRoot "..\local\certs"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path -LiteralPath $OutDir).Path

$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -KeyUsage DigitalSignature `
    -KeyExportPolicy Exportable `
    -CertStoreLocation Cert:\CurrentUser\My `
    -NotAfter (Get-Date).AddYears(3)

$secure = ConvertTo-SecureString $Password -AsPlainText -Force
$pfx = Join-Path $OutDir "bypass_tool-test.pfx"
$cer = Join-Path $OutDir "bypass_tool-test.cer"

Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $secure | Out-Null
Export-Certificate -Cert $cert -FilePath $cer | Out-Null

if ($Trust) {
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher | Out-Null
    Import-Certificate -FilePath $cer -CertStoreLocation Cert:\CurrentUser\Root | Out-Null
}

Write-Output ("thumbprint: " + $cert.Thumbprint)
Write-Output ("pfx: " + $pfx)
Write-Output ("cer: " + $cer)
if ($Trust) { Write-Output "installed into CurrentUser TrustedPublisher + Root" }
