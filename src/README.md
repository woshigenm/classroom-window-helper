# 源码说明

| 文件 | 目标 | 编译脚本 / make 目标（在仓库根目录执行） |
|---|---|---|
| `bypass.c` | 32 位主程序（GUI + 托盘 + 注入调度 + 看门狗） | `build\build_bypass.bat` / `make.bat bypass` |
| `elevate.c` / `elevate.h` | 启动提权（`lpe` / `uac` / `manual` / `off`，配置 `bin\bypass_elevate.ini`） | 随 `bypass.c` 一起编译 |
| `lpe_broker.c` | 自动提权的 `bin\lpe\shell_broker.exe`：以管理员身份拉起 `bin\bypass.exe` | `make.bat lpe` |
| `bypass_hook.c` | 32 位注入 DLL（IAT + 内联钩子、窗口子类化、最小化锁） | `build\build_hook32.bat` / `make.bat hook32` |
| `bypass_vd.inc` | 虚拟桌面固定模块（纯 ASCII，被 `bypass_hook.c` 用 `#include` 挂进来） | 随 `bypass_hook.c` 一起编译 |
| `inject32.c` | 32 位注入器（按进程名 / `--pid`） | 同上 |
| `bypass_state.h` | 主程序 / DLL 共享状态（配置文件 + 共享内存，32/64 位布局一致） | 随两个 `.c` 一起编译 |

编译产物统一输出到仓库根目录的 `bin\`；自测载体源码在 `..\tests\hooktest32.c`，窗口模式小窗的对照测试见 `..\tests\wmode_test.c`（`make.bat wmodetest`），虚拟桌面固定的回归测试见 `..\tests\vd_pin_test.c`（`make.bat vdtest`）。
产物是 `bin\hooktest32.exe`（`make.bat test`）。

`elevate.c` 只做“怎么拿到管理员权限”，拿到之后与老版本完全一致：`Elevate_Boot()` 返回后
要么已经拉起一个管理员实例（当前进程退出），要么已经弹出/打印了手动指引。
`lpe_broker.c` 是 32 位、无窗口的控制台程序，作为按需服务被 WalletService 拉起；
它只启动 `command.txt` 里给出的那一行命令（构建时由 `bin\bypass.exe` 写入）。

## bypass_hook.c 模块顺序

1. 路径 / 进程名 / 教师端判定
2. 日志（UTF-8 格式串 → ACP，`FILE_APPEND_DATA` 追加，跨进程安全；主程序每次启动
   用 `CREATE_ALWAYS` 截断 `bypass_hook.log`，所以文件里只有**本次运行**的记录）
3. 窗口属性与用户输入判定（`UserRestoreSignal`、任务栏/预览/菜单）
4. 标题栏自动隐藏（`CaptionApply` / `CaptionTick`）
5. 模拟最大化 / 还原（`ToggleEmulatedMax`、`MaxRectFor`、`RestoreManagedWindow`）
6. 窗口子类化（`SubclassProc`：几何冻结、任务栏最小化锁、`SC_*` 处理）
7. IAT 钩子（`ShowWindow/Async`、`SetWindowPos`、`SetWindowLong*`、`SetWindowPlacement`、`CreateProcess*`…）
8. 内联钩子（`FollowJump`、`InstallInlineHook`：真函数 5 字节 `E9` + trampoline）
9. Worker 循环（enforcer、桌面守护、注入扩散、IAT 周期补丁）
10. 小窗尺寸配置、崩溃取证、`DllMain`
11. 窗口模式广播小窗：应用出生就带置顶的小广播窗，由 `EnforceProc` 投递 `WM_BYPASS_FIX(force=2)` 在窗口线程里摘掉 `WS_EX_TOPMOST`（日志 `untopmost`）；之后应用再重申置顶时，`SmallBcastNoRaise()` 让它只冻结 Z 序（`after=NULL` + `SWP_NOZORDER`），不再把窗口抬到用户窗口之上。详见 `docs\design.md` 2.4。

`bypass_vd.inc`（在第 7 步之前 `#include`，提供 `VD_Start()` 与 `VD_PinNow()`）：
虚拟桌面 COM（`IVirtualDesktopManager`）、`bypass_vd.ini` 读写、注册表桌面表、
1 Hz 兜底扫描，以及 `CreateWindowExW` / `SetForegroundWindow` / `BringWindowToTop` /
`SetActiveWindow` 四个钩子。

## 线程与锁

- `g_logCs`：日志写入；不与其他锁反向嵌套。
- `g_subCs`：子类化表；持锁期间只做属性读取 / `PostMessage`，**不写日志、不跨线程 ShowWindow**。
- `CaptionPumpProc`（250ms）：只 `PostMessage`（`WM_BYPASS_CAP` / `WM_BYPASS_REMIN`），
  真正的窗口操作都回到窗口线程。
- `Worker`（enforcer）：跨进程只做判断，修改一律 `PostMessage(WM_BYPASS_FIX)` 回窗口线程。
- `g_vd` 系列的 COM 只在 `VD_Thread`（私有 STA）上用，**绝不在 `DllMain` 或渲染器线程上
  `CoInitializeEx`**；钩子线程只把窗口投递过去并最多等 250ms。
- `g_user` 只被窗口线程访问；`g_geo` 的并发写只影响诊断日志。

## 关键不变量

- 三个运行文件都在 `..\bin\`，必须同目录（DLL 按自身路径找 `bypass_config.ini`、`bypass_hook.off` 和日志）。
- 虚拟桌面固定只认 `MultiRender*` / `StreamRenderWindow*` / `OeScreenRenderWindow*` / `SDL_app`
  这四类窗口，别把学生端界面窗（`StudentHelperWnd.oseasy`）当成渲染窗。
- 教师端进程（Teacher / ScreenSender / …）在打任何补丁前就退出 Worker。
- 内联钩子只接受“整 5 字节单指令”的序言（`8B FF 55 8B EC` / `B8 imm32` / `68 imm32`）。
- 任务栏最小化锁只在窗口线程设置/解除；解锁的合法来源：
  `WA_CLICKACTIVE`、任务栏/预览点击、`SC_RESTORE/SC_MAXIMIZE`（带用户特征）、
  `WM_BYPASS_UNPARK`、`WM_WINDOWPOSCHANGED`（已确认 `!IsIconic`）、250ms 心跳兜底。
- 用户恢复统一走 `RestoreManagedWindow`：解锁 → `ShowWindow` → 按模拟最大化 / 用户矩形摆放。
