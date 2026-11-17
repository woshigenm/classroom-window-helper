# 更新记录

本文件随发布包一起分发，只记录中文。更早版本见 GitHub Releases 页面。

## v2.1.0（2026-11-17）

### 修复

- **自动提权（LPE）100% 静默失败**：载荷投递机制重做。`trigger.ps1` 长过 19 KB 后，
  UTF-16 + Base64 塞不进 32767 字符的命令行上限，每次 LPE 都在编码处失败并退回 UAC，
  日志只有一行 `encoded command does not fit`。现在脚本连同前置变量落盘到
  `%LOCALAPPDATA%\ClassroomBypass-LPE\trigger.<pid>.<tick>.ps1`（UTF-8 带 BOM），用
  `-Command "Invoke-Expression (Get-Content ...)"` 执行——执行策略（含 GPO 锁死的
  `Restricted`，真机验证）管不到 `-Command` 的文本。落盘脚本由启动器清理，硬杀残留由
  下次同 pid 运行或 `build\cleanup_lpe.ps1` 清掉。真机链路 0.8~6 s 出握手行。
- **「跳过」从硬杀改为协作退出**：点「跳过」写 `abort.<pid>` 文件，脚本在轮询里看到就
  `return`，由 `finally` 恢复 Documents；3 秒没反应才 `TerminateProcess`。脚本放进
  Job Object（`KILL_ON_JOB_CLOSE`），启动器被强杀时脚本跟着死；正常返回先撤掉这个
  限制再关句柄（撤不掉就故意泄漏句柄）。真机验收：协作退出 47 ms、卡死 3.0 s 硬杀、
  快照修复、skip 落在成功之后不再弹第二个 UAC。
- **Documents 重定向的三处真实缺陷**：快照机制重做（读已知文件夹改为进程内 +
  exit code 判定；快照写到 `%LOCALAPPDATA%\ClassroomBypass-LPE\documents.orig`；
  recorded / guess / 硬失败三分，快照目录不可达直接 `exit 4` 拒绝运行）。
- **屏幕监控守卫的假 off 根因**：探测窗口类名 `MultiRender*` 命中渲染窗白名单前缀被
  接管，`HWND_TOPMOST` 被改写。探针改名 + 新增双向契约测试（`tests\rclass_test.c`），
  收紧匹配会红测试而不是静默改行为。
- 一批小缺陷：`qTarget[900]` 静默截断（`--elevate=uac` 被截掉）；BOM 剥离 `memmove`
  越界读；命令行装不下时拒绝启动而不是截断着启动；Job disarm 失败不再关句柄（会杀掉
  正在跑 `finally` 的脚本）；守护关→开会服务旧帧（现在重新定格）；抓屏尺寸中途变化
  后定格帧拉伸铺满而不是缩在角落。

### 新增

- **托盘菜单「屏蔽屏幕监控（定格帧）」开关**（勾选态、Ctrl+Alt+M 提示），与主界面
  按钮同一开关；武装时主界面**自动收进托盘**——Windows 更新后 GDI 抓屏把「对抓屏
  排除的窗口」渲染成黑色（防泄露加固），界面可见时武装会把黑洞烤进定格帧。
- **`build\cleanup_lpe.ps1`**：默认只列不删（CVE49176_* 服务 / run 目录 /
  `documents.orig` / `abort.*` / `trigger.*.ps1`，判活不看服务名里的 pid——那是
  WalletService 的）；`-RunId` / `-Service` / `-All -Force`（net session + 输入
  DELETE）按需清理；`documents.orig` 任何参数下都不删。
- **freeze forensics**：定格帧亮度统计 + 原样 BMP dump 到 `%TEMP%`；守护开关写日志。
- **「检查当前状态」末尾新增 Shared Log 增量段**：字节偏移锚点，把提权链路 / payload /
  DLL 的行（只进文件的那部分）搬进界面，轮转自动从头读。
- `--elevate-selftest` 重做：落盘脚本 + 完整命令行写入日志（不再产出 `.b64`），排查
  LPE 回退从这两行开始。
- 真机验收记录：`docs\P1-abort.md` §0/§8（哪些单元覆盖、哪些真机跑过、哪些没有）。

### 改进

- 等待循环双时钟拆分（脚本退出后只看自己的宽限，不再被 60 s 总上限截断）；
  握手/child_pid 采信前核对映像名（防复用 pid）。
- 测试载体不再抢键盘焦点（`WS_EX_NOACTIVATE` + `SW_SHOWNOACTIVATE` +
  `SWP_NOACTIVATE`；`core_host` 的 hammer 线程不再每 50 ms 抢一次）。
- PoC 载荷的三个源码进入 `lpe\src\`（供应商原样 + `mingw` 重建目标），payload 与
  源码不一致时拒绝铺装。
- 全套测试 15 套 72+ 项，`make all` 0 警告。

## v2.0.0（2026-11-17）

### 新增

- **Loader / Core 两层**：`bypass_hook32.dll` 只负责装钩子（inline + IAT）、持有
  trampoline、日志、路径和控制管道；所有拦截逻辑移到 `bypass_hook_core.dll`。
- **热重载 Core**：主界面「重载 Core」/ 托盘「重载 Core（热更新）」，或控制管道
  `\\.\pipe\bypass_core_ctl_<pid>` 的 `status` / `reload` / `reload norollback`。
  只改 Core 逻辑时**不需要重启课堂客户端**。
- **重载安全**：`core.<pid>.<seq>.dll` 缓存副本、失败自动回滚、两阶段卸载
  （阶段 1 只读检查、阶段 2 不可逆清理）、Loader 侧权威 drain、`still mapped` 检测。
  失败/半禁用会明确提示，终态 `coreDead` 只能重启进程。
- **版本自检改用内容哈希**（FNV-1a）：能区分「Core 旧 → 点重载」和
  「Loader 旧 → 重启客户端」，并能识别「构建正在重写 DLL」。
- **多开/并发验证**：4 个课堂进程一次重载、控制管道并发、崩溃隔离。
- `make.bat core`：只编 Core，不动 Loader，避免版本自检误报。
- `core_cache` 孤儿自愈：Loader 启动时清理已退出进程留下的缓存副本。

### 改进

- 半禁用语义收敛：重载失败后 22 个入口直通，只剩安全类（抓屏冻结、关机入口、
  低层输入钩子抑制）继续生效，UI 文案逐项列出。
- 界面：重载期间状态栏显示 `reloading Core (up to 15s)`，只有明确的 `err` 才算失败。
- 「检查当前状态」的结论分三档：Core 旧 → 点重载；Loader 旧/半禁用 → 重启客户端；
  磁盘文件正在被构建重写 → 稍后再查。
- 日志清空改为代数校验，清空后不会被后台线程排队中的旧行「复活」。

### 修复

- 重载成功后 `g_unloading` 未复位，导致所有转发器永久直通（现场现象：桌面被切到
  `NetTeachingDesktop`、`lpDesktop` 不再清、输入钩子不再抑制）。
- 卸载旧 Core 后 `status` 仍读 `g_core.build_id`（指向已卸载模块，UAF）。
- 控制管道 `ERROR_PIPE_BUSY` 被当成「没注入/旧 DLL」。
- Core 初始化与卸载的竞态：Worker 在阶段 2 之后仍可能创建 caption/VD 线程，
  导致阶段 2 超时；用 `g_initDepth` 握手修复。
- 子类恢复失败时不再清空 `g_sub[]`，下一次重载会继续重试。
- `PeStampLoaded` 缺少 `TH32CS_SNAPMODULE32`，64 位主程序枚举 32 位课堂进程的模块
  永远失败（旧单 DLL 的兜底路径形同虚设）。
- drain 原用 `3000 × Sleep(1)`（Windows 下约 45 秒），改为墙钟 deadline。
- LPE 提权：上一次中断留下的「文档」重定向会被当成原始路径反复恢复；
  `trigger.ps1` 现在检测到 LPE 临时目录就直接改用 `%USERPROFILE%\Documents`。
- 缺少 `bin\lpe\` 时不再先尝试 LPE（会白等一次 Wallet 请求）而是直接走 UAC；
  发布包也会带上完整的 `bin\lpe\`。

### 兼容性

- `HR_ABI_VERSION = 3`、`BYPASS_STATE_VERSION = 1` 未变；v1.2.0 的 Loader 不能与
  v2.0.0 的 Core 混用——**升级请整套替换 `bin\` 并重启课堂客户端**。
- 回退：用 `bin\bypass_hook_single.dll` 覆盖 `bypass_hook32.dll`，重启课堂客户端。
