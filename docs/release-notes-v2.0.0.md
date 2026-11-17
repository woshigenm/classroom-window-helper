# classroom-window-helper v2.0.0

v2.0.0 把注入 DLL 拆成 **Loader + Core**，改拦截逻辑不再需要重启课堂客户端；
同时修掉了重载路径上的几个真实缺陷（hook 全哑、`status` UAF、管道 busy 误判）。

v2.0.0 splits the injected DLL into **Loader + Core** so hook logic can be hot-reloaded
without restarting the classroom client, and fixes several real defects on that path
(dead hooks after reload, `status` UAF, pipe-busy misclassification).

---

## 中文

### 新增

- **Loader / Core 两层**：`bypass_hook32.dll` 装钩子（inline + IAT）、持有 trampoline、
  日志、路径和控制管道；拦截逻辑全部移到 `bypass_hook_core.dll`。
- **热重载 Core**：界面「重载 Core」/ 托盘项，或管道 `status` / `reload` /
  `reload norollback`；只改 Core 时不用重启客户端。
- **重载安全**：缓存副本、失败回滚、两阶段卸载、Loader 侧 drain、`still mapped` 检测；
  失败会明确提示，`coreDead` 是终态（只能重启进程）。
- **版本自检改用内容哈希**：区分「Core 旧 → 点重载」与「Loader 旧 → 重启」，
  并能识别「构建正在重写 DLL」。
- **多开/并发验证**：4 进程一次重载、管道并发、崩溃隔离。
- `make.bat core`（只编 Core）；`core_cache` 孤儿自愈。

### 修复

- 重载成功后 `g_unloading` 未复位 → 所有转发器永久直通（现场：桌面被切到
  `NetTeachingDesktop`、`lpDesktop` 不再清、输入钩子不再抑制）。
- 卸载旧 Core 后 `status` 读悬空 `g_core.build_id`（UAF）。
- 管道 `ERROR_PIPE_BUSY` 被当成「没注入」。
- Core 初始化与卸载的线程竞态（阶段 2 超时）。
- 子类恢复失败清表导致下一次误判成功。
- `PeStampLoaded` 缺 `TH32CS_SNAPMODULE32`（旧 DLL 兜底永远失败）。
- drain 用 `3000 × Sleep(1)`（实际约 45 秒）→ 墙钟 deadline。
- LPE 中断留下的「文档」重定向被反复恢复；`trigger.ps1` 已加守卫。

### 升级

- `HR_ABI_VERSION = 3` / `BYPASS_STATE_VERSION = 1` 未变，但 **Loader 与 Core 必须整套
  一起换**；升级请替换整个 `bin\` 并重启课堂客户端。
- 提权模式由代码决定（有 in\lpe\ 走 LPE，否则 UAC），没有 ypass_elevate.ini 这个配置文件。
- 回退：ypass_hook_single.dll 覆盖 ypass_hook32.dll 后重启客户端。

---

## English

### Added

- **Loader / Core split**: `bypass_hook32.dll` installs the inline/IAT hooks, owns the
  trampolines, logging, paths and the control pipe; every interception lives in
  `bypass_hook_core.dll`.
- **Hot reload**: "Reload Core" button / tray item, or the control pipe
  (`status`, `reload`, `reload norollback`). Core-only changes no longer need a
  classroom-client restart.
- **Reload safety**: cached copy per process, automatic rollback on failure,
  two-phase unload, authoritative Loader-side drain, `still mapped` check;
  `coreDead` is terminal (restart the process).
- **Version check by content hash**: distinguishes "Core is stale → click reload"
  from "Loader is stale → restart", and detects "the DLL is being rebuilt".
- **Multi-instance / concurrency coverage**: four classroom processes reloaded with one
  click, concurrent pipe clients, crash isolation.
- `make.bat core` (Core only); orphaned `core_cache` files are cleaned automatically.

### Fixed

- `g_unloading` was not reset after a successful reload, so every dispatcher stayed in
  passthrough (field symptoms: input desktop switched to `NetTeachingDesktop`,
  `lpDesktop` no longer cleared, low-level input hooks no longer suppressed).
- `status` read a dangling `g_core.build_id` after the old Core was unloaded (UAF).
- `ERROR_PIPE_BUSY` was treated as "not injected".
- Thread-creation race between Core init and unload (phase-2 timeout).
- Subclass-restore failure cleared the table, so the next reload wrongly succeeded.
- `PeStampLoaded` missed `TH32CS_SNAPMODULE32` (the old-DLL fallback never worked).
- The drain used `3000 × Sleep(1)` (~45 s on Windows); now a wall-clock deadline.
- LPE left a Documents redirect behind and restored the stale value forever;
  `trigger.ps1` now detects it.

### Upgrade

- `HR_ABI_VERSION = 3` / `BYPASS_STATE_VERSION = 1` are unchanged, but the **Loader and
  Core must be replaced together**: replace the whole `bin\` and restart the classroom
  client.
- The elevation mode is decided by the code (in\lpe\ present -> LPE, else UAC); there
  is no ypass_elevate.ini.  Rollback: copy ypass_hook_single.dll over
  ypass_hook32.dll, then restart the
  classroom client.

---

## Package / 包内容

```
bin\bypass.exe              main UI / tray
bin\bypass_hook32.dll       Loader
bin\bypass_hook_core.dll    Core (hot-reload target)
bin\bypass_hook_single.dll  rollback: old single-DLL build
bin\inject32.exe            injector
bin\lpe\                    auto-elevation payload (trigger.ps1 + wallet helpers)
bin\bypass_config.ini       runtime config
bin\bypass_shut.ini         remote shutdown guard
docs\how-to-use.md          usage
docs\design.md              internals
README.md
RUN.txt
CHANGELOG.md                (中文 / Chinese only)
```

SHA256 `classroom-window-helper-v2.0.0-bin.zip`:
`70A874246427FA6F5B8FABA1EF0FC770917B0078F4A3483163F57E4BC618DB00`
