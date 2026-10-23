# 源码说明

| 文件 | 目标 | 编译脚本 / make 目标（在仓库根目录执行） |
|---|---|---|
| `bypass.c` | 64 位主程序（GUI + 托盘 + 注入调度 + 看门狗） | `build\build_bypass.bat` / `make.bat bypass` |
| `bypass_hook.c` | 32 位注入 DLL（IAT + 内联钩子、窗口子类化、最小化锁） | `build\build_hook32.bat` / `make.bat hook32` |
| `inject32.c` | 32 位注入器（按进程名 / `--pid`） | 同上 |
| `bypass_state.h` | 主程序 / DLL 共享状态（配置文件 + 共享内存，32/64 位布局一致） | 随两个 `.c` 一起编译 |

编译产物统一输出到仓库根目录的 `bin\`；自测载体源码在 `..\tests\hooktest32.c`，
产物是 `bin\hooktest32.exe`（`make.bat test`）。

## bypass_hook.c 模块顺序

1. 路径 / 进程名 / 教师端判定
2. 日志（UTF-8 格式串 → ACP，`FILE_APPEND_DATA` 追加，跨进程安全）
3. 窗口属性与用户输入判定（`UserRestoreSignal`、任务栏/预览/菜单）
4. 标题栏自动隐藏（`CaptionApply` / `CaptionTick`）
5. 模拟最大化 / 还原（`ToggleEmulatedMax`、`MaxRectFor`、`RestoreManagedWindow`）
6. 窗口子类化（`SubclassProc`：几何冻结、任务栏最小化锁、`SC_*` 处理）
7. IAT 钩子（`ShowWindow/Async`、`SetWindowPos`、`SetWindowLong*`、`SetWindowPlacement`、`CreateProcess*`…）
8. 内联钩子（`FollowJump`、`InstallInlineHook`：真函数 5 字节 `E9` + trampoline）
9. Worker 循环（enforcer、桌面守护、注入扩散、IAT 周期补丁）
10. 小窗尺寸配置、崩溃取证、`DllMain`

## 线程与锁

- `g_logCs`：日志写入；不与其他锁反向嵌套。
- `g_subCs`：子类化表；持锁期间只做属性读取 / `PostMessage`，**不写日志、不跨线程 ShowWindow**。
- `CaptionPumpProc`（250ms）：只 `PostMessage`（`WM_BYPASS_CAP` / `WM_BYPASS_REMIN`），
  真正的窗口操作都回到窗口线程。
- `Worker`（enforcer）：跨进程只做判断，修改一律 `PostMessage(WM_BYPASS_FIX)` 回窗口线程。
- `g_user` 只被窗口线程访问；`g_geo` 的并发写只影响诊断日志。

## 关键不变量

- 三个运行文件都在 `..\bin\`，必须同目录（DLL 按自身路径找 `bypass_config.ini`、`bypass_hook.off` 和日志）。
- 教师端进程（Teacher / ScreenSender / …）在打任何补丁前就退出 Worker。
- 内联钩子只接受“整 5 字节单指令”的序言（`8B FF 55 8B EC` / `B8 imm32` / `68 imm32`）。
- 任务栏最小化锁只在窗口线程设置/解除；解锁的合法来源：
  `WA_CLICKACTIVE`、任务栏/预览点击、`SC_RESTORE/SC_MAXIMIZE`（带用户特征）、
  `WM_BYPASS_UNPARK`、`WM_WINDOWPOSCHANGED`（已确认 `!IsIconic`）、250ms 心跳兜底。
- 用户恢复统一走 `RestoreManagedWindow`：解锁 → `ShowWindow` → 按模拟最大化 / 用户矩形摆放。
