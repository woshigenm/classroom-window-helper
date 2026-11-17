# cleanup_lpe.ps1 -- inspect, and only on request remove, what an interrupted LPE
# run can leave behind.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File build\cleanup_lpe.ps1
#       list everything; changes nothing.  This is the default for a reason.
#
#   ... -Service CVE49176_1234_567890     delete one service (needs admin)
#   ... -RunId 0123abcd...                delete one run directory
#   ... -All -Force                       delete every ORPHAN item (needs admin,
#                                         net session, and typing DELETE)
#
# Why this is not a one-liner:
#
#  * The pid inside CVE49176_<pid>_<tick> is NOT the elevated instance.  It is
#    GetCurrentProcessId() of the process hosting wallet_callback_shell.dll,
#    i.e. WalletService (SYSTEM).  That process is alive nearly always, so
#    "Get-Process -Id <pid>" would label every leftover [RUNNING] forever and
#    nothing would ever be cleanable.  Liveness is decided from the service
#    state and from processes whose image lives inside the run directory.
#
#  * documents.orig is never deleted here, not even by -All.  It is the only
#    copy of the user's real Documents path after a run that died before
#    restoring it; throwing it away turns a recoverable state into a permanent
#    one.  Removing it is a decision for a human who has checked the path.
#
#  * services go before directories: deleting a directory first leaves a
#    service pointing at a missing file, which is worse than leaving both.

[CmdletBinding()]
param(
    [string]$Service,
    [string]$RunId,
    [switch]$All,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$lpeRoot = Join-Path $env:LOCALAPPDATA 'ClassroomBypass-LPE'

function Test-Admin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p  = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# All process paths in one call.  Fields can be empty for processes this
# account may not inspect; an empty field is "unknown", never "absent".
function Get-ProcessSnapshot {
    try {
        return @(Get-CimInstance Win32_Process -ErrorAction Stop |
                 Select-Object Name, ProcessId, ExecutablePath, CommandLine)
    } catch {
        return @()
    }
}

function Test-PathUnder([string]$path, [string]$dir) {
    if (-not $path -or -not $dir) { return $false }
    return $path.Trim('"').StartsWith($dir, [StringComparison]::OrdinalIgnoreCase)
}

# "C:\...\payload\shell_broker.exe" "C:\...\payload\result.txt"  ->  run dir
function Get-RunDirFromImagePath([string]$imagePath) {
    if (-not $imagePath) { return $null }
    $m = [regex]::Match($imagePath, '^\s*"([^"]+)"')
    if (-not $m.Success) {
        $m = [regex]::Match($imagePath, '^\s*(\S+\.exe)')
        if (-not $m.Success) { return $null }
    }
    $exe = $m.Groups[1].Value
    $payload = Split-Path -Parent $exe
    if (-not $payload) { return $null }
    return (Split-Path -Parent $payload)
}

function Get-LiveIn([object[]]$procs, [string]$dir) {
    $hits = @()
    if (-not $dir) { return $hits }
    foreach ($p in $procs) {
        if ((Test-PathUnder $p.ExecutablePath $dir) -or
            ($p.CommandLine -and $p.CommandLine.IndexOf($dir, [StringComparison]::OrdinalIgnoreCase) -ge 0)) {
            $hits += ("{0}({1})" -f $p.Name, $p.ProcessId)
        }
    }
    return $hits
}

function Get-ServiceRows([object[]]$procs) {
    $rows = @()
    $names = @()
    try {
        $names = @(Get-CimInstance Win32_Service -ErrorAction Stop |
                   Where-Object { $_.Name -like 'CVE49176_*' } |
                   Select-Object Name, State, ProcessId, PathName)
    } catch {
        Write-Host "  cannot enumerate services with CIM: $($_.Exception.Message)" -ForegroundColor Yellow
        return $rows
    }
    foreach ($s in $names) {
        $runDir = Get-RunDirFromImagePath $s.PathName
        $live = @(Get-LiveIn $procs $runDir)
        $state = [string]$s.State
        $why = @()
        if ($state -ne 'Stopped') { $why += "state=$state" }
        if ($live.Count -gt 0)    { $why += ("processes=" + ($live -join ',')) }
        $label = if ($why.Count -gt 0) { 'RUNNING, do not delete' } else { 'ORPHAN' }
        $pidHint = ($s.Name -split '_')[1]
        $rows += [pscustomobject]@{
            Name   = $s.Name
            State  = $state
            PidHint = $pidHint
            RunDir = $runDir
            Label  = $label
            Why    = ($why -join ' ')
            Path   = [string]$s.PathName
        }
    }
    return $rows
}

function Show-Services([object[]]$rows) {
    Write-Host ""
    Write-Host "== CVE49176_* services ==" -ForegroundColor Cyan
    Write-Host "   pid= is the wallet host (WalletService), not the elevated instance;"
    Write-Host "   it is not a liveness signal.  Liveness = service state + run-dir processes."
    if ($rows.Count -eq 0) { Write-Host "   (none)"; return }
    foreach ($r in $rows) {
        Write-Host ("   {0}  pid={1}  state={2}  [{3}]  runid={4}" -f
                    $r.Name, $r.PidHint, $r.State, $r.Label,
                    ($(if ($r.RunDir) { $r.RunDir } else { '?' })))
        Write-Host ("      path={0}" -f $r.Path)
        if ($r.Why) { Write-Host ("      why={0}" -f $r.Why) }
    }
}

function Get-DirRows([object[]]$procs) {
    $rows = @()
    if (-not (Test-Path -LiteralPath $lpeRoot)) { return $rows }
    foreach ($d in (Get-ChildItem -LiteralPath $lpeRoot -Directory -ErrorAction SilentlyContinue)) {
        $live = @(Get-LiveIn $procs $d.FullName)
        $files = @(Get-ChildItem -LiteralPath $d.FullName -Recurse -File -ErrorAction SilentlyContinue)
        $size = ($files | Measure-Object -Property Length -Sum).Sum
        if (-not $size) { $size = 0 }
        $rows += [pscustomobject]@{
            Name  = $d.Name
            Path  = $d.FullName
            Files = $files.Count
            Bytes = $size
            Label = if ($live.Count -gt 0) { 'RUNNING, do not delete' } else { 'ORPHAN' }
            Why   = if ($live.Count -gt 0) { 'processes=' + ($live -join ',') } else { '' }
        }
    }
    return $rows
}

function Show-Dirs([object[]]$rows) {
    Write-Host ""
    Write-Host "== run directories under $lpeRoot ==" -ForegroundColor Cyan
    if ($rows.Count -eq 0) { Write-Host "   (none)"; return }
    foreach ($r in $rows) {
        Write-Host ("   {0}  files={1} size={2:N0} B  [{3}]" -f
                    $r.Name, $r.Files, $r.Bytes, $r.Label)
        if ($r.Why) { Write-Host ("      why={0}" -f $r.Why) }
    }
}

function Show-Snapshot {
    Write-Host ""
    Write-Host "== documents.orig (never deleted by this script) ==" -ForegroundColor Cyan
    $snap = Join-Path $lpeRoot 'documents.orig'
    if (-not (Test-Path -LiteralPath $snap)) { Write-Host "   (none)"; return }
    $value = ''
    try { $value = [IO.File]::ReadAllText($snap, [Text.Encoding]::UTF8).Trim() } catch { $value = '<unreadable>' }
    Write-Host ("   value={0}" -f $value)
    Write-Host "   If this file exists, a run died before restoring Documents."
    Write-Host "   Check the real path first; if it is wrong, repair with:"
    Write-Host "     bin\lpe\documents_path.exe set <real path>"
}

function Show-AbortFiles {
    Write-Host ""
    Write-Host "== abort.* (skip signal; the launcher deletes its own) ==" -ForegroundColor Cyan
    if (-not (Test-Path -LiteralPath $lpeRoot)) { Write-Host "   (none)"; return }
    $found = @(Get-ChildItem -LiteralPath $lpeRoot -File -Filter 'abort.*' -ErrorAction SilentlyContinue)
    if ($found.Count -eq 0) { Write-Host "   (none)"; return }
    $procs = Get-ProcessSnapshot
    foreach ($f in $found) {
        $pidText = $f.Name.Substring(6)
        $byPid = $procs | Where-Object { [string]$_.ProcessId -eq $pidText }
        # pid alone is not proof: only bypass.exe writes this file, so a live
        # pid with any other image is a reused pid, and the file is a leftover.
        $alive = $byPid | Where-Object { $_.Name -ieq 'bypass.exe' }
        if (-not $alive -and $byPid) {
            Write-Host ("   {0}  [ORPHAN, pid {1} reused by {2}]" -f $f.Name, $pidText, $byPid[0].Name)
        } elseif ($alive) {
            Write-Host ("   {0}  [RUNNING, do not delete]" -f $f.Name)
            Write-Host ("      why=pid {0} is {1}" -f $pidText, $alive[0].Name)
        } else {
            Write-Host ("   {0}  [ORPHAN]" -f $f.Name)
        }
    }
}

function Show-Scripts {
    Write-Host ""
    Write-Host "== trigger.*.ps1 (payload scripts staged for a run) ==" -ForegroundColor Cyan
    if (-not (Test-Path -LiteralPath $lpeRoot)) { Write-Host "   (none)"; return }
    $found = @(Get-ChildItem -LiteralPath $lpeRoot -File -Filter 'trigger.*.ps1' -ErrorAction SilentlyContinue)
    if ($found.Count -eq 0) { Write-Host "   (none)"; return }
    $procs = Get-ProcessSnapshot
    foreach ($f in $found) {
        # trigger.<pid>.<tick>.ps1 - the pid is the launcher's, and only
        # bypass.exe writes these (same pid-reuse caveat as abort.*).
        $pidText = ($f.Name -split '\.')[1]
        $byPid = $procs | Where-Object { [string]$_.ProcessId -eq $pidText }
        $alive = $byPid | Where-Object { $_.Name -ieq 'bypass.exe' }
        if (-not $alive -and $byPid) {
            Write-Host ("   {0}  {1:N0} B  [ORPHAN, pid {2} reused by {3}]" -f
                        $f.Name, $f.Length, $pidText, $byPid[0].Name)
        } elseif ($alive) {
            Write-Host ("   {0}  {1:N0} B  [RUNNING, do not delete]" -f $f.Name, $f.Length)
        } else {
            Write-Host ("   {0}  {1:N0} B  [ORPHAN]" -f $f.Name, $f.Length)
        }
    }
    Write-Host "   Present = the launcher died between staging and deleting."
    Write-Host "   It is a copy of the script, not state: deleting one loses nothing."
}

function Remove-OneService([string]$name, [bool]$force) {
    if ($name -notmatch '^CVE49176_[0-9]+_[0-9]+$') {
        throw "refusing '$name': not a CVE49176_<pid>_<tick> name"
    }
    $svc = Get-CimInstance Win32_Service -Filter ("Name='" + $name + "'") -ErrorAction SilentlyContinue
    if (-not $svc) { Write-Host "   $name : not present"; return }
    if (-not $force -and $svc.State -ne 'Stopped') {
        Write-Host "   $name : state=$($svc.State) - refusing (use -Force if you are sure)" -ForegroundColor Yellow
        return
    }
    & sc.exe delete $name | Out-Host
    if ($LASTEXITCODE -eq 0) { Write-Host "   $name : deleted" -ForegroundColor Green }
    else { Write-Host "   $name : sc delete failed ($LASTEXITCODE)" -ForegroundColor Red }
}

function Remove-OneDir([string]$path, [bool]$force) {
    if (-not (Test-Path -LiteralPath $path)) { Write-Host "   $path : not present"; return }
    if (-not $force) {
        $live = @(Get-LiveIn (Get-ProcessSnapshot) $path)
        if ($live.Count -gt 0) {
            Write-Host "   $path : in use by $($live -join ',') - refusing" -ForegroundColor Yellow
            return
        }
    }
    Remove-Item -LiteralPath $path -Recurse -Force
    Write-Host "   $path : removed" -ForegroundColor Green
}

# ---- listing (always) --------------------------------------------------------

$procs = Get-ProcessSnapshot
$svcRows = @(Get-ServiceRows $procs)
$dirRows = @(Get-DirRows $procs)

Write-Host "LPE leftovers on this machine ($env:COMPUTERNAME, $(Get-Date -Format s))"
if (-not (Test-Admin)) {
    Write-Host "Not elevated: services can be listed, and nothing can be deleted." -ForegroundColor Yellow
}
Show-Services $svcRows
Show-Dirs $dirRows
Show-Snapshot
Show-AbortFiles
Show-Scripts

# ---- explicit cleanup --------------------------------------------------------

if ($Service) {
    if (-not (Test-Admin)) { throw 'deleting a service needs an elevated prompt' }
    Write-Host ""
    Write-Host "== delete service $Service ==" -ForegroundColor Cyan
    Remove-OneService $Service ([bool]$Force)
}

if ($RunId) {
    if ($RunId -notmatch '^[0-9a-fA-F]{32}$') {
        throw "refusing RunId '$RunId': expected the 32-hex directory name"
    }
    Write-Host ""
    Write-Host "== delete run directory $RunId ==" -ForegroundColor Cyan
    Remove-OneDir (Join-Path $lpeRoot $RunId) ([bool]$Force)
}

if ($All) {
    Write-Host ""
    Write-Host "== clean every ORPHAN item ==" -ForegroundColor Cyan
    if (-not (Test-Admin)) { throw '-All needs an elevated prompt (net session must succeed)' }
    if (-not $Force)   { throw '-All also needs -Force' }
    & net session 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'net session failed - run this from an elevated prompt' }

    $busy = @($svcRows | Where-Object { $_.Label -ne 'ORPHAN' }) +
            @($dirRows | Where-Object { $_.Label -ne 'ORPHAN' })
    if ($busy.Count -gt 0) {
        foreach ($b in $busy) { Write-Host ("   active: {0}" -f $b.Name) -ForegroundColor Yellow }
        throw 'something is still running - stop the tool before -All'
    }

    Write-Host "This deletes: CVE49176_* services (stopped), run directories,"
    Write-Host "and stale abort.* / trigger.*.ps1 whose pid no longer runs."
    Write-Host "It does NOT delete documents.orig - that is the only copy of the"
    Write-Host "user's real Documents path until a restore succeeds."
    $answer = Read-Host "Type DELETE to continue"
    if ($answer -ne 'DELETE') { Write-Host '   cancelled'; return }

    # Services first: a directory deleted under a live service leaves the
    # service pointing at a missing file.
    foreach ($r in $svcRows) { Remove-OneService $r.Name $true }
    foreach ($r in $dirRows) { Remove-OneDir $r.Path $true }

    foreach ($f in (Get-ChildItem -LiteralPath $lpeRoot -File -Filter 'abort.*' -ErrorAction SilentlyContinue)) {
        $pidText = $f.Name.Substring(6)
        $alive = $procs | Where-Object { [string]$_.ProcessId -eq $pidText -and $_.Name -ieq 'bypass.exe' }
        if ($alive) { Write-Host "   $($f.Name) : launcher pid $pidText alive - kept" }
        else { Remove-Item -LiteralPath $f.FullName -Force; Write-Host "   $($f.Name) : removed" }
    }

    foreach ($f in (Get-ChildItem -LiteralPath $lpeRoot -File -Filter 'trigger.*.ps1' -ErrorAction SilentlyContinue)) {
        $pidText = ($f.Name -split '\.')[1]
        $alive = $procs | Where-Object { [string]$_.ProcessId -eq $pidText -and $_.Name -ieq 'bypass.exe' }
        if ($alive) { Write-Host "   $($f.Name) : launcher pid $pidText alive - kept" }
        else { Remove-Item -LiteralPath $f.FullName -Force; Write-Host "   $($f.Name) : removed" }
    }
}

if (-not ($Service -or $RunId -or $All)) {
    Write-Host ""
    Write-Host "Nothing was changed.  Cleanup arguments:" -ForegroundColor Cyan
    Write-Host "  -Service <CVE49176_pid_tick>   delete one stopped service"
    Write-Host "  -RunId   <32-hex>              delete one run directory"
    Write-Host "  -All -Force                    delete every ORPHAN item (admin)"
}
