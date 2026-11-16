# multi_test.ps1 -- multi-instance / concurrency coverage (the dimension P2 never
# exercised):
#
#   1. four classroom processes at once, one UI click reloads all of them
#   2. per-process cache copies, the old *.1.dll deleted, hashes still match disk
#   3. two clients hammering the same control pipe: every request gets a real
#      answer, never "no control pipe" (ERROR_PIPE_BUSY is waited on)
#   4. a status check during a 3 s reload window must not advise "restart"
#   5. a crash in one process (after its reload) is logged for that pid only and
#      the other three keep answering (UEF stack / blob store are per-process)
#
# Run from the repo root after `make.bat all testcores corehost uitest`.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$log  = Join-Path $bin 'bypass_hook.log'
$dll  = Join-Path $bin 'bypass_hook32.dll'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.multi.dll'
$prev = $null
$slow = $null
$victim = $null
$hosts = @()
$pass = $true

Add-Type -Namespace W -Name N -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, System.Text.StringBuilder l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
public static uint HashFile(string path) {
    uint h = 2166136261u;
    foreach (byte b in System.IO.File.ReadAllBytes(path)) { h ^= b; h *= 16777619u; }
    return h;
}
'@

function Send-Ctl([int]$procId, [string]$msg, [int]$timeoutMs = 20000) {
    $name = "bypass_core_ctl_$procId"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    while ($true) {
        try {
            $pipe = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name,
                        [System.IO.Pipes.PipeDirection]::InOut)
            $pipe.Connect(200)
            break
        } catch {
            if ($sw.ElapsedMilliseconds -gt $timeoutMs) { return $null }
            Start-Sleep -Milliseconds 50
        }
    }
    try {
        $bytes = [System.Text.Encoding]::ASCII.GetBytes($msg)
        $pipe.Write($bytes, 0, $bytes.Length)
        $pipe.Flush()
        $buf = New-Object byte[] 1024
        $n = $pipe.Read($buf, 0, $buf.Length)
        if ($n -le 0) { return '' }
        return [System.Text.Encoding]::ASCII.GetString($buf, 0, $n).Trim()
    } finally { $pipe.Dispose() }
}

function Start-Host([string]$exeName, [int]$seconds, [switch]$Window, [int]$CrashAfterMs = 0) {
    $pidfile = Join-Path $env:TEMP ("bypass_multi_" + $exeName + ".pid")
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    $exe = Join-Path $bin $exeName
    # a leftover host of the same name would keep the image locked
    Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($exeName)) -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 20; $i++) {
        try { Copy-Item (Join-Path $bin 'core_host.exe') $exe -Force; break }
        catch { Start-Sleep -Milliseconds 250 }
    }
    if (!(Test-Path $exe)) { throw "cannot stage $exeName" }
    $argv = @('--pidfile', $pidfile, '--inject', $dll, '--seconds', "$seconds")
    if ($Window) { $argv += '--window' }
    if ($CrashAfterMs -gt 0) { $argv += @('--crash-after', "$CrashAfterMs") }
    $p = Start-Process -FilePath $exe -WorkingDirectory $bin -PassThru -ArgumentList $argv
    for ($i = 0; $i -lt 150 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw "$exeName did not write its pidfile" }
    $procId = [int](Get-Content $pidfile -Raw).Trim()
    return [pscustomobject]@{ Name = $exeName; Proc = $p; Pid = $procId; Pidfile = $pidfile }
}

function Find-UiWindow([int]$procId) {
    $script:top = [IntPtr]::Zero
    $cb = [W.N+EnumProc]{
        param($h, $l)
        $x = 0
        [void][W.N]::GetWindowThreadProcessId($h, [ref]$x)
        if ($x -eq $procId -and [W.N]::IsWindowVisible($h)) { $script:top = $h; return $false }
        return $true
    }
    [void][W.N]::EnumWindows($cb, [IntPtr]::Zero)
    return $script:top
}

function Get-UiEdit([IntPtr]$top) {
    $script:edit = [IntPtr]::Zero
    $cb = [W.N+EnumProc]{
        param($h, $l)
        $sb = New-Object System.Text.StringBuilder 64
        [void][W.N]::GetClassName($h, $sb, 64)
        if ($sb.ToString() -eq 'Edit') { $script:edit = $h; return $false }
        return $true
    }
    [void][W.N]::EnumChildWindows($top, $cb, [IntPtr]::Zero)
    return $script:edit
}

function Read-UiLog([IntPtr]$edit) {
    $sb = New-Object System.Text.StringBuilder 262144
    [void][W.N]::SendMessage($edit, 0x000D, [IntPtr]262144, $sb)
    return $sb.ToString()
}

function Get-DiskHash([string]$path) {
    # FNV-1a 32, same as HrFnv1aFile
    return ('{0:x8}' -f [W.N]::HashFile($path))
}

function Stop-Host($h) {
    if ($null -eq $h) { return }
    if ($h.Proc -and !$h.Proc.HasExited) { Stop-Process -Id $h.Proc.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $h.Pidfile -Force -ErrorAction SilentlyContinue
    # the image stays locked for a moment after Stop-Process returns
    $exe = Join-Path $bin $h.Name
    for ($i = 0; $i -lt 20 -and (Test-Path $exe); $i++) {
        Remove-Item $exe -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 100
    }
}

Copy-Item $core $save -Force
try {
    # ---------- 1. four classroom processes ----------
    $names = @('Student.exe', 'MultiClient.exe', 'ScreenRecord.exe', 'ScreenRender.exe')
    foreach ($n in $names) { $hosts += Start-Host $n 70 -Window }
    Start-Sleep -Milliseconds 1500

    "--- four hosts ---"
    foreach ($h in $hosts) {
        $st = Send-Ctl $h.Pid 'status'
        "  $($h.Name) pid=$($h.Pid): $st"
        if (-not $st -or $st -notmatch '^ok .* seq=1 dead=0') { "    FAIL: not ready"; $pass = $false }
    }

    $diskHash = Get-DiskHash $core
    foreach ($h in $hosts) {
        $st = Send-Ctl $h.Pid 'status'
        if ($st -notmatch "hash=$diskHash") { "    FAIL: $($h.Name) hash != disk"; $pass = $false }
    }

    # ---------- 2. one UI click reloads all four ----------
    $prev = Start-Process -FilePath (Join-Path $bin 'ui_preview.exe') -PassThru
    Start-Sleep -Milliseconds 1800
    $top  = Find-UiWindow $prev.Id
    if ($top -eq [IntPtr]::Zero) { throw 'ui_preview window not found' }
    $edit = Get-UiEdit $top
    [void][W.N]::PostMessage($top, 0x0111, [IntPtr]1017, [IntPtr]::Zero)   # ID_BTN_RELOADCORE
    Start-Sleep -Milliseconds 7000

    $ui = Read-UiLog $edit
    "--- UI reload over four processes ---"
    @($ui -split "`r?`n" | Where-Object { $_ -match '\[Core\]' }) | ForEach-Object { $_ }
    $okLines = @($ui -split "`r?`n" | Where-Object { $_ -match ': ok seq=2' })
    if ($okLines.Count -ne 4) { "  FAIL: expected 4 'ok seq=2', got $($okLines.Count)"; $pass = $false }
    if ($ui -notmatch 'ok=4 rolledback=0 refused=0 skipped=0 dead=0') { '  FAIL: summary is not ok=4'; $pass = $false }

    "--- after reload ---"
    foreach ($h in $hosts) {
        $st = Send-Ctl $h.Pid 'status'
        "  $($h.Name) pid=$($h.Pid): $st"
        if ($st -notmatch 'seq=2 dead=0') { "    FAIL: seq/dead"; $pass = $false }
        if ($st -notmatch "hash=$diskHash") { "    FAIL: hash changed"; $pass = $false }
        if (Get-ChildItem (Join-Path $bin "core_cache\core.$($h.Pid).1.dll") -ErrorAction SilentlyContinue) {
            "    FAIL: old cache still on disk"; $pass = $false
        }
        if (-not (Get-ChildItem (Join-Path $bin "core_cache\core.$($h.Pid).2.dll") -ErrorAction SilentlyContinue)) {
            "    FAIL: new cache missing"; $pass = $false
        }
    }

    # ---------- 3. two clients, same pipe ----------
    $target = $hosts[0]
    "--- concurrent clients on pid=$($target.Pid) ---"
    $jobs = @()
    foreach ($k in 1..2) {
        $jobs += Start-Job -ScriptBlock {
            param($procId)
            $name = "bypass_core_ctl_$procId"
            $sw = [System.Diagnostics.Stopwatch]::StartNew()
            while ($true) {
                try {
                    $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name,
                            [System.IO.Pipes.PipeDirection]::InOut)
                    $p.Connect(200); break
                } catch {
                    if ($sw.ElapsedMilliseconds -gt 20000) { return 'no-connect' }
                    Start-Sleep -Milliseconds 50
                }
            }
            try {
                $b = [Text.Encoding]::ASCII.GetBytes('reload')
                $p.Write($b, 0, $b.Length); $p.Flush()
                $buf = New-Object byte[] 1024
                $n = $p.Read($buf, 0, 1024)
                if ($n -le 0) { return 'empty' }
                return [Text.Encoding]::ASCII.GetString($buf, 0, $n).Trim()
            } finally { $p.Dispose() }
        } -ArgumentList $target.Pid
    }
    $res = @()
    foreach ($j in $jobs) {
        $res += (Wait-Job $j -Timeout 40 | Receive-Job)
        Remove-Job $j -Force -ErrorAction SilentlyContinue
    }
    foreach ($r in $res) { "  reply: $r" }
    if (($res | Where-Object { $_ -match '^ok seq=' }).Count -lt 1) { '  FAIL: no reload succeeded'; $pass = $false }
    if ($res | Where-Object { $_ -eq 'no-connect' -or $_ -eq 'empty' }) { '  FAIL: a client got no answer'; $pass = $false }
    $st = Send-Ctl $target.Pid 'status'
    if ($st -notmatch 'dead=0') { "  FAIL: target disabled: $st"; $pass = $false }

    # ---------- 4. status check during a 3 s reload ----------
    "--- status during a slow reload (stuck worker core) ---"
    # free the ScreenRender.exe slot so the UI's status report actually lists the
    # slow host (the report only scans Student/MultiClient/ScreenRecord/ScreenRender)
    Stop-Host $hosts[3]
    $hosts[3] = $null
    Copy-Item (Join-Path $bin 'core_stuck_worker.dll') $core -Force
    $slow = Start-Host 'ScreenRender.exe' 45 -Window
    Start-Sleep -Milliseconds 1500
    $job = Start-Job -ScriptBlock {
        param($procId)
        $name = "bypass_core_ctl_$procId"
        $p = New-Object System.IO.Pipes.NamedPipeClientStream('.', $name,
                [System.IO.Pipes.PipeDirection]::InOut)
        $p.Connect(5000)
        try {
            $b = [Text.Encoding]::ASCII.GetBytes('reload')
            $p.Write($b, 0, $b.Length); $p.Flush()
            $buf = New-Object byte[] 1024
            $n = $p.Read($buf, 0, 1024)
            if ($n -le 0) { return 'empty' }
            return [Text.Encoding]::ASCII.GetString($buf, 0, $n).Trim()
        } finally { $p.Dispose() }
    } -ArgumentList $slow.Pid
    Start-Sleep -Milliseconds 500        # reload is inside phase 2 (3 s)

    [void][W.N]::PostMessage($top, 0x0111, [IntPtr]1007, [IntPtr]::Zero)   # ID_BTN_CHECK_STATUS
    Start-Sleep -Milliseconds 3500
    $ui2 = Read-UiLog $edit
    $slowLine = @($ui2 -split "`r?`n" | Where-Object { $_ -match 'ScreenRender' })
    "  (UI lines for ScreenRender.exe)"
    $slowLine | ForEach-Object { "  $_" }
    if ($slowLine -match 'restart the classroom client') { '  FAIL: restart advised mid-reload'; $pass = $false }
    if ($slowLine -match 'not loaded|not injected') { '  FAIL: reported as not injected'; $pass = $false }

    $slowRes = (Wait-Job $job -Timeout 40 | Receive-Job)
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    "  slow reload reply: $slowRes"
    if (-not $slowRes -or $slowRes -eq 'empty' -or $slowRes -eq 'no-connect') {
        "  FAIL: slow reload got no answer"; $pass = $false
    }
    if ($slowRes -match 'core disabled|coreDead') { "  FAIL: stuck worker must only fail phase 2"; $pass = $false }

    # ---------- 5. crash isolation ----------
    "--- UEF isolation: a fifth process crashes after its reload ---"
    Copy-Item $save $core -Force
    $victim = Start-Host 'SharedDesktop.exe' 40 -Window -CrashAfterMs 9000
    Start-Sleep -Milliseconds 1500
    $st = Send-Ctl $victim.Pid 'reload'
    "  victim reload: $st"
    if ($st -notmatch '^ok seq=2') { '  FAIL: reload before the crash'; $pass = $false }

    $gone = $false
    for ($i = 0; $i -lt 60; $i++) {
        Start-Sleep -Milliseconds 250
        if (!(Get-Process -Id $victim.Pid -ErrorAction SilentlyContinue)) { $gone = $true; break }
    }
    $crash = Select-String -Path $log -Pattern '!! crash' |
             Where-Object { $_.Line -match "\[pid $($victim.Pid)\]" } |
             Select-Object -Last 1
    "  crash logged: " + $(if ($crash) { $crash.Line.Trim() } else { 'NONE' })
    if (-not ($gone -and $crash)) { '  FAIL: crash was not handled by the new Core'; $pass = $false }

    foreach ($h in @($hosts[0], $hosts[1], $hosts[2])) {
        $st = Send-Ctl $h.Pid 'status'
        if (-not $st -or $st -notmatch 'dead=0') { "  FAIL: $($h.Name) disturbed: $st"; $pass = $false }
        else { "  $($h.Name): still ok" }
    }
} finally {
    foreach ($h in $hosts) { Stop-Host $h }
    Stop-Host $slow
    Stop-Host $victim
    if ($prev -and !$prev.HasExited) { Stop-Process -Id $prev.Id -Force -ErrorAction SilentlyContinue }
    Copy-Item $save $core -Force
    Remove-Item $save -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (multi-instance reload, pipe concurrency, crash isolation)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
