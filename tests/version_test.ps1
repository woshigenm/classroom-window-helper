# version_test.ps1 -- P3 version self-check (content hash, not PE stamp).
#
# Drives the real UI ("检查当前状态" = ID 1007) through bin\ui_preview.exe:
#   1. unchanged disk        -> "core <hash> (current)", no advice
#   2. disk core replaced    -> "<== OLD (disk <hash>)" + "(click 重载 Core,
#                               no restart)" and never "restart the classroom
#                               client" (a Core-only change needs no restart)
#   3. disk rewritten in a loop -> never advises restart (a build in progress
#                               must not be mistaken for a stale Core)
#
# Run from the repo root after `make.bat all testcores corehost uitest`.

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$dll  = Join-Path $bin 'bypass_hook32.dll'
$core = Join-Path $bin 'bypass_hook_core.dll'
$save = Join-Path $bin 'bypass_hook_core.p3test.dll'
$pass = $true
$prev = $null
$host0 = $null

Add-Type -Namespace W -Name N -MemberDefinition @'
public delegate bool EnumProc(IntPtr h, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, System.Text.StringBuilder l);
[DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, System.Text.StringBuilder s, int n);
'@

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

function Click-Check([IntPtr]$top) {
    [void][W.N]::PostMessage($top, 0x0111, [IntPtr]1007, [IntPtr]::Zero)   # ID_BTN_CHECK_STATUS
    Start-Sleep -Milliseconds 2500
}

$pidfile = Join-Path $env:TEMP 'bypass_version_host.pid'
Copy-Item $core $save -Force
try {
    Get-Process -Name 'ScreenRender' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    for ($i = 0; $i -lt 20; $i++) {
        try { Copy-Item (Join-Path $bin 'core_host.exe') (Join-Path $bin 'ScreenRender.exe') -Force; break }
        catch { Start-Sleep -Milliseconds 250 }
    }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    $host0 = Start-Process -FilePath (Join-Path $bin 'ScreenRender.exe') -WorkingDirectory $bin -PassThru `
        -ArgumentList @('--pidfile', $pidfile, '--inject', $dll, '--window', '--seconds', '90')
    for ($i = 0; $i -lt 80 -and !(Test-Path $pidfile); $i++) { Start-Sleep -Milliseconds 100 }
    if (!(Test-Path $pidfile)) { throw 'ScreenRender.exe did not start' }
    Start-Sleep -Milliseconds 1200

    $prev = Start-Process -FilePath (Join-Path $bin 'ui_preview.exe') -PassThru
    Start-Sleep -Milliseconds 1800
    $top  = Find-UiWindow $prev.Id
    if ($top -eq [IntPtr]::Zero) { throw 'ui_preview window not found' }
    $edit = Get-UiEdit $top

    # ---- 1. unchanged disk ----
    Click-Check $top
    $lines = @((Read-UiLog $edit) -split "`r?`n" | Where-Object { $_ -match 'ScreenRender|core on disk|all classroom|WARNING' })
    "--- 1. current ---"
    $lines | ForEach-Object { $_ }
    if (-not ($lines -match 'core [0-9a-f]{8} \(current\)')) { '  FAIL: not reported as current'; $pass = $false }
    if ($lines -match 'restart the classroom client') { '  FAIL: advice given while current'; $pass = $false }

    # ---- 2. core replaced on disk: OLD, advice = reload ----
    Copy-Item (Join-Path $bin 'core_evil.dll') $core -Force
    Click-Check $top
    $lines = @((Read-UiLog $edit) -split "`r?`n" | Where-Object { $_ -match 'ScreenRender|core on disk|WARNING' })
    "--- 2. disk replaced ---"
    $lines | ForEach-Object { $_ }
    if (-not ($lines -match '<== OLD \(disk [0-9a-f]{8}\)')) { '  FAIL: OLD not detected'; $pass = $false }
    if (-not ($lines -match 'no restart')) { '  FAIL: no reload advice'; $pass = $false }
    if ($lines -match 'restart the classroom client') { '  FAIL: restart advised for a Core-only change'; $pass = $false }

    # ---- 3. rewrite in progress must never advise restart ----
    Copy-Item $save $core -Force
    $job = Start-Job -ScriptBlock {
        param($path)
        for ($k = 0; $k -lt 12; $k++) {
            $src = if ($k % 2) { $path + '.a' } else { $path + '.b' }
            $bytes = [System.IO.File]::ReadAllBytes($src)
            $fs = [System.IO.File]::Open($path, 'Create', 'Write', 'ReadWrite')
            try {
                $fs.Write($bytes, 0, 40000)
                $fs.Flush()
                Start-Sleep -Milliseconds 120
                $fs.Write($bytes, 40000, $bytes.Length - 40000)
                $fs.Flush()
            } finally { $fs.Dispose() }
            Start-Sleep -Milliseconds 60
        }
    } -ArgumentList $core
    Copy-Item (Join-Path $bin 'core_evil.dll') ($core + '.a') -Force
    Copy-Item $save ($core + '.b') -Force

    $saw = @()
    for ($n = 0; $n -lt 3; $n++) {
        Click-Check $top
        $saw += @((Read-UiLog $edit) -split "`r?`n" | Where-Object { $_ -match 'ScreenRender.exe ' })
        if ($saw -match 'restart the classroom client') { '  FAIL: restart advised during a disk rewrite'; $pass = $false }
    }
    Wait-Job $job -Timeout 10 | Out-Null
    Remove-Job $job -Force -ErrorAction SilentlyContinue
    Remove-Item ($core + '.a'), ($core + '.b') -Force -ErrorAction SilentlyContinue
    "--- 3. rewrite in progress (never advises restart) ---"
    $saw | ForEach-Object { "  $_" }
} finally {
    if ($prev -and !$prev.HasExited) { Stop-Process -Id $prev.Id -Force -ErrorAction SilentlyContinue }
    if ($host0 -and !$host0.HasExited) { Stop-Process -Id $host0.Id -Force -ErrorAction SilentlyContinue }
    Remove-Item $pidfile -Force -ErrorAction SilentlyContinue
    Remove-Item (Join-Path $bin 'ScreenRender.exe') -Force -ErrorAction SilentlyContinue
    Copy-Item $save $core -Force
    Remove-Item $save -Force -ErrorAction SilentlyContinue
}

"VERDICT " + $(if ($pass) { 'PASS (hash-based status: current / OLD+reload / no false restart)' } else { 'FAIL' })
if (-not $pass) { exit 1 }
