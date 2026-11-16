# ============================================================
#  foreign_fix_test.ps1 -- "跨进程兜底不能卡死 enforcer" 的现场测试
#
#  背景: bypass_hook32.dll 的 enforcer 线程过去会同步 SetWindowLong/
#  SetWindowPos 到别的课堂进程的窗口。对端 UI 线程一旦不泵消息, 那个
#  SendMessage 就永远不返回 -> 整个 enforcer 线程 (小窗化 / 注入 / 心跳)
#  停摆。现在改成 SWP_ASYNCWINDOWPOS + 不碰样式。
#
#  做法:
#    1) 把 bin\hangwin_test.exe 复制成 <tmp>\ScreenRender.exe 并启动 ——
#       它建一个 MultiRender* + 95% 屏的窗口, 然后挂起自己的主线程
#       (永不泵消息);
#    2) 在 bin\ 里跑 wmode_test --inject (它已载入我们的 DLL, enforcer 会
#       看到那个外来窗口);
#    3) 断言: wmode_test 在超时前跑完、自身通过, 且日志里有 fix(foreign, async)。
#
#  注: 同步版的实际阻塞时间实测约 64 秒 (probe: 跨进程 SetWindowPos 到一个
#  挂起线程的窗口), 但在本测试的时序里不一定每次都撞满 —— 所以判据取
#  "有没有走异步路径 + 宿主是否还在跑", 而不是"卡了多少秒"。
#
#  用法: powershell -NoProfile -ExecutionPolicy Bypass -File tests\foreign_fix_test.ps1
#  ⚠ 本文件必须存成 "UTF-8 with BOM" (PowerShell 5.1 否则按 ANSI 读)。
# ============================================================

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $root 'bin'
$work = Join-Path $env:TEMP 'bypass-foreign-test'
$log  = Join-Path $bin 'bypass_hook.log'

foreach ($p in @((Join-Path $bin 'wmode_test.exe'),
                 (Join-Path $bin 'bypass_hook32.dll'),
                 (Join-Path $bin 'inject32.exe'),
                 (Join-Path $bin 'hangwin_test.exe'))) {
    if (-not (Test-Path $p)) { throw "missing $p - run: make.bat all hangwintest" }
}

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Path $work | Out-Null

# 受害者必须叫 ScreenRender.exe, 注入方才会把它当同类课堂进程
$victim = Join-Path $work 'ScreenRender.exe'
Copy-Item (Join-Path $bin 'hangwin_test.exe') $victim -Force

if (Test-Path $log) { Remove-Item $log -Force }
Remove-Item (Join-Path $work 'victim.log') -ErrorAction SilentlyContinue

$vp = Start-Process -FilePath $victim -WorkingDirectory $work -PassThru -WindowStyle Hidden

# 等它把窗口建好并挂起
$ready = Join-Path $work 'victim.log'
$deadline = (Get-Date).AddSeconds(10)
while (-not (Test-Path $ready) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 100 }
if (-not (Test-Path $ready)) {
    Stop-Process -Id $vp.Id -Force -ErrorAction SilentlyContinue
    throw 'victim never became ready'
}
Write-Host ("victim: " + (Get-Content $ready -Raw).Trim())

# 受害者必须是 64 位: 我们的注入器/内联钩子是 32 位的, 一旦把 DLL 注进受害者,
# PidHasDll() 就会让 enforcer 走"对端自己有钩子"的分支, 跨进程兜底根本不会执行。
$vbits = (Select-String -Path $ready -Pattern 'bits=(\d+)' -AllMatches).Matches[0].Groups[1].Value
if ($vbits -ne '64') {
    Stop-Process -Id $vp.Id -Force -ErrorAction SilentlyContinue
    Write-Host "SKIP: 受害者是 $vbits 位 (需要 64 位工具链), 本机无法验证跨进程分支"
    exit 0
}

$fail = $false
$outFile = Join-Path $work 'wmode.out'
try {
    $hp = Start-Process -FilePath (Join-Path $bin 'wmode_test.exe') `
            -ArgumentList '--ticks 3 --inject bypass_hook32.dll' `
            -WorkingDirectory $bin -PassThru -NoNewWindow `
            -RedirectStandardOutput $outFile

    if (-not $hp.WaitForExit(60000)) {
        Write-Host 'FAIL: wmode_test 卡住了(>60s) - enforcer 线程被对端阻塞'
        try { Stop-Process -Id $hp.Id -Force -ErrorAction SilentlyContinue } catch {}
        $fail = $true
    } else {
        $wmodeOut = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { '' }
        if ($wmodeOut -match 'VERDICT FIXED') {
            Write-Host 'wmode_test: VERDICT FIXED (宿主 enforcer 一直在跑)'
        } else {
            Write-Host 'FAIL: wmode_test 自己没通过'
            if ($wmodeOut) { Write-Host $wmodeOut }
            $fail = $true
        }
    }
} finally {
    Stop-Process -Id $vp.Id -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path $log)) {
    Write-Host 'FAIL: 没有生成 bypass_hook.log'
    exit 1
}

$hit = Select-String -Path $log -Pattern 'fix(foreign, async)' -SimpleMatch
if ($hit) {
    Write-Host ("log: " + $hit[0].Line.Trim())
} else {
    Write-Host 'FAIL: 日志里没有 fix(foreign, async) - 跨进程兜底没跑到'
    $fail = $true
}

if ($fail) { Write-Host 'VERDICT FAIL'; exit 1 }
Write-Host 'VERDICT PASS (foreign fix stayed async, enforcer kept running)'
exit 0
