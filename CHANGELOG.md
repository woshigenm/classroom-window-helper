# 更新记录

本文件随发布包一起分发，只记录中文。更早版本见 GitHub Releases 页面。

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
