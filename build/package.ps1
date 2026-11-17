param(
    [string]$Version = "v2.0.0",
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

$required = @("bypass.exe", "bypass_hook32.dll", "bypass_hook_core.dll", "inject32.exe")
$optional = @("bypass_hook_single.dll")          # rollback: copy over bypass_hook32.dll
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
    foreach ($name in $optional) {
        $p = Join-Path $Bin $name
        if (Test-Path -LiteralPath $p) {
            Copy-Item -LiteralPath $p -Destination $stageBin
        } else {
            Write-Warning ("optional file missing (rollback will not be available): " + $name)
        }
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

    # Runtime ini files that carry explicit (default) settings.  bypass_vd.ini is
    # deliberately NOT shipped: it stores the pinned desktop GUID of the machine
    # that produced the zip.
    foreach ($ini in @("bypass_shut.ini")) {
        $p = Join-Path $Bin $ini
        if (Test-Path -LiteralPath $p) { Copy-Item -LiteralPath $p -Destination $stageBin }
    }

    # Auto-elevation payload (bin\lpe\trigger.ps1 + wallet helpers).  Without it
    # the launcher can only fall back to UAC, so ship the whole tree.
    $lpeSrc = Join-Path $Bin "lpe"
    if (Test-Path -LiteralPath $lpeSrc) {
        Copy-Item -LiteralPath $lpeSrc -Destination $stageBin -Recurse -Force
        # Older builds kept the Documents snapshot in bin\lpe (it now lives
        # under %LOCALAPPDATA%).  A tree that still has one would ship this
        # machine's Documents path to the recipient's first run.
        $stale = Join-Path $stageBin "lpe\documents.orig"
        if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }
    } else {
        Write-Warning "bin\lpe missing: the package can only elevate through UAC"
    }

    # Keep the docs\ subfolder: README links to docs/design.md and
    # docs/how-to-use.md, so flattening them would break the links.
    $stageDocs = Join-Path $stage "docs"
    New-Item -ItemType Directory -Path $stageDocs -Force | Out-Null
    foreach ($rel in @("README.md", "CHANGELOG.md", "docs\how-to-use.md", "docs\design.md")) {
        $src = Join-Path $Root $rel
        if (Test-Path -LiteralPath $src) {
            $dst = if ($rel.StartsWith("docs\")) { $stageDocs } else { $stage }
            Copy-Item -LiteralPath $src -Destination $dst
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

    @(
        "Classroom Window Mode v2.0"
        ""
        "1. Extract this zip (bin\ must stay on a local disk)."
        "2. Run bin\bypass.exe as Administrator."
        "3. Config: bin\bypass_config.ini"
        "4. Kill switch: create bin\bypass_hook.off (then restart the classroom app)."
        ""
        "Files:"
        "  bin\bypass.exe            main UI / tray"
        "  bin\bypass_hook32.dll     Loader (hooks, hot reload control pipe)"
        "  bin\bypass_hook_core.dll  Core (all hook logic; hot-reloadable)"
        "  bin\bypass_hook_single.dll  old single-DLL build, rollback only:"
        "                            copy it over bypass_hook32.dll and restart"
        "                            the classroom client"
        "  bin\inject32.exe          injector"
        ""
        "Hot reload (change Core logic without restarting the classroom app):"
        "  make.bat core  ->  click 'Reload Core' in the UI (or the tray item)."
        "  Loader changes still need a classroom-client restart."
        "  See README.md 5.6 / docs\design.md 2.9."
        ""
        "Docs: docs\how-to-use.md (usage), docs\design.md (internals)."
        ""
        "Authorized lab use only."
    ) | Set-Content -LiteralPath (Join-Path $stage "RUN.txt") -Encoding ASCII

    New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
    $zip = Join-Path $OutDir ("classroom-window-helper-" + $Version + "-bin.zip")
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
    Write-Output ("zip: " + $zip)
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
}
