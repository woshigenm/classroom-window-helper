# 设计与实现

> 本文说明「小窗化广播」的核心机制、开关、日志、自测和已知限制。
> 使用方法见 [`how-to-use.md`](how-to-use.md)。

---

## 1. 工作原理

### 1.1 三条"硬手段"（在 `bypass_hook32.dll`，注入到 32 位课堂进程后生效）

| # | 目标 API | 动作 | 目的 |
|---|---|---|---|
| 1 | `CreateProcessA/W` | 把命令行里的 `#fullscreen#:1` 改写成 `0`，并给子进程加环境变量 `BYPASS_BROADCAST=1` | 渲染器**从创建时**就是窗口模式，不是"事后抢" |
| 2 | `CreateProcessA/W` | 清空 `STARTUPINFO.lpDesktop = "NetTeachingDesktop"` | 不让渲染器被建到隐藏桌面（否则学生这边看不到画面） |
| 3 | `SwitchDesktop` | 直接返回 `TRUE`（假装成功） | 不切到空的教学桌面 |
| 4 | `SetWindowsHookEx(WH_MOUSE_LL / WH_KEYBOARD_LL)` | 返回假句柄 | 课堂客户端会装全局低级钩子只放行它自己的事件，真实鼠标键盘全废 |

以上都在 `bypass_config.ini` 的 `window_mode=0` 时关闭；**默认开启**。

### 1.2 窗口层：让广播窗口变成"用户的小窗"

- **IAT Hook**（`SetWindowPos` / `MoveWindow` / `SetWindowPlacement` / `ShowWindow` /
  `ShowWindowAsync` / `SetWindowLong*` / `SetCursor` / `ShowCursor` / `CreateProcess*` /
  `SwitchDesktop` / `SetWindowsHookEx`）——逐模块扫描导入表并改写。
- **窗口子类化**（`SetWindowLongPtr(GWLP_WNDPROC)`）：`WM_GETMINMAXINFO` 放开最小尺寸、
  `WM_NCCALCSIZE` 补回标准外框、`WM_STYLECHANGED` 补回标题栏、`WM_SETCURSOR` 兜光标。
- 窗口的"身份"用窗口属性标记：`BypassSmall`（已接管）、`BypassOldProc`（原窗口过程）、
  `BypassWantW/H`（目标尺寸）、`BypassUserRc`（用户矩形，见 1.4）。

### 1.3 怎么判断"哪个窗口才是广播窗"（身份优先，尺寸兜底）

尺寸猜测会误伤（实测：教室软件的「虚拟桌面」预览窗是 75% 屏 + 置顶 + 带框，很容易被
当成广播窗一起降级，屏幕上就出现两个小窗）。所以按优先级：

| 级别 | 证据 | 看尺寸吗 |
|---|---|---|
| 1 | 环境变量 `BYPASS_BROADCAST=1`（父进程被注入时由我们写入） | 否 |
| 2 | 渲染器自己的命令行里还有 `#fullscreen#:1`（父进程未被注入） | 否 |
| 3 | 窗口类是渲染器类（`MultiRender*` / `StreamRenderWindow*` / `OeScreenRenderWindow` / `SDL_app`），或进程镜像是 `ScreenRender.exe` / `MultiRender.exe` / `SharedDesktop.exe` | 否 |
| 4 | 已带 `BypassSmall` 属性（已接管） | 否 |
| 5 | 以上都没有 → 才用几何兜底：宽高都 ≥ 屏幕 90%，且**带标题栏的普通窗口一律不碰** | 是 |

`StudentHelperWnd.oseasy`（学生端浮动工具条/虚拟桌面面板）因此完全不会被接管。

### 1.4 几何与窗口状态：全部按"用户意图"判定

窗口被接管后，机器要回答两个问题：**这次几何变化是谁发起的？这次最大化/还原是谁要的？**

| 信号 | 来源 | 用途 |
|---|---|---|
| `BypassSizing` | `WM_ENTERSIZEMOVE` ~ `WM_EXITSIZEMOVE` | 用户正在拖边框/拖标题栏 → 一切放行 |
| `BypassUserCmd` | `WM_SYSCOMMAND`(SC_RESTORE/MAXIMIZE/MINIMIZE/MOVE/SIZE)、`WM_NCLBUTTONDBLCLK` | 用户刚下过命令（一次命令只放行一次状态切换） |
| `BypassUserRc` | 降级完成时 / 每次 `WM_EXITSIZEMOVE` | "用户矩形"：还原的目标，不会被应用缓存尺寸污染 |
| `BypassZoomed` | `WM_WINDOWPOSCHANGED` | 记录当前是否最大化 |

规则：

- **用户拖动**（`BypassSizing`）→ 不插手任何东西。
- **应用发起的几何变更**（`SetWindowPos` / `MoveWindow` / `WM_WINDOWPOSCHANGING`）→
  统一改成 `SWP_NOSIZE | SWP_NOMOVE`（并吞掉 `SWP_FRAMECHANGED`，否则边框重算会把
  `WS_MAXIMIZE` 丢掉）。
- **应用发起的窗口状态变更**（`ShowWindow` / `ShowWindowAsync` 的
  `SW_RESTORE/SW_SHOWNORMAL/SW_MAXIMIZE/SW_SHOWMAXIMIZED`）→ **一律忽略**。
- **用户的最大化/还原** → 由我们在窗口线程里直接执行：
  - `SC_MAXIMIZE` / 双击标题栏 → **模拟最大化**：不设 `WS_MAXIMIZE`，把外框铺到
    `rcMonitor` 外一圈（边框推到屏幕外，客户区正好铺满整屏），并打 `BypassUserMax`；
  - 再点一次 / `SC_RESTORE` → 摘掉 `BypassUserMax`、补回标题栏，并
    `SetWindowPos(用户矩形)`。
  这样即使应用把 `WS_MAXIMIZE` 位改乱，用户也总能把窗口摆回他要的样子。

> 为什么不用 `WS_MAXIMIZE` 当判据：渲染器每 500ms 会把缓存的旧样式写回来（不含
> `WS_MAXIMIZE`），一旦该位被清掉，窗口就变成「普通窗口 + 满屏几何」——标题栏被顶到
> 屏幕外 `(-8,-8)`，最大化/还原按钮既看不见也点不到，用户看到的就是"窗口卡死"。

### 1.5 线程安全：绝不跨线程改窗口

`enforcer` 只做判断，真正的修改用 `PostMessage(WM_APP+0x51)` 丢回**窗口所属线程**执行。
早期版本在 enforcer 线程直接对别人的窗口调 `SetWindowPos`，会和窗口线程互相等待
（自测里第 2 轮就整窗卡死）。

### 1.6 日志：多进程并发安全

`bypass_hook.log` 会被 3~4 个进程同时写。用 `CreateFileA(FILE_APPEND_DATA)` +
单次 `WriteFile` 追加（文件系统保证原子追加），行尾 `\r\n`，编码 ACP（中文系统即 GBK）。
早期用 `fopen(...,"a")` 是"先 seek 到末尾再写"，多进程会撞同一偏移，导致**半行被吃掉、
换行丢失**。

日志**每次运行覆盖**：主程序在注入前用
`CreateFileA(..., CREATE_ALWAYS, FILE_SHARE_READ|WRITE|DELETE)` 把 `bypass_hook.log` 截断
（`TruncateHookLog()`，`src/bypass.c`）。不用 `DeleteFile` 是因为上一轮残留的 3~4 个进程
还握着追加句柄 —— 删掉再重建会让它们写进一个已经脱离目录的旧文件，本轮日志就丢行。
截断只发生在单实例检查之后，即只有真正启动的那一次会清空。

### 1.7 键盘解锁：只保留手动

实测发现"检测到广播/小窗化成功就自动发 `0xB2008`"不可靠（驱动不在/被占用时只是刷失败日志），
**自动解锁已移除**。键盘解锁只剩手动路径：

- 主界面「解锁键盘」按钮（`UnlockKeyboard()`，先原生 IOCTL，失败退回厂商 DLL / rundll32）。

> 不再提供 `Ctrl+Alt+K` 热键：全屏广播时键盘通常被驱动锁死，快捷键根本按不出来；
> 键盘没锁时又不需要解锁。需要解锁时用鼠标点按钮（鼠标被冻结时先按 `Ctrl+Alt+E` 应急点击）。

日志里不再出现 `[Keyboard] broadcast is now a small window -> auto unlock ...`。

### 1.8 最大化后标题栏自动隐藏

只对**用户把广播小窗最大化**（`BypassUserMax`）的窗口生效：

| 事件 | 动作 |
|---|---|
| 鼠标离开标题栏 ≥ 1.2 秒 | 摘掉 `WS_CAPTION`（保留 `WS_THICKFRAME`）→ 标题栏连同最小化/最大化/关闭一起消失，客户区铺满 |
| 鼠标进入窗口顶边 8px | 立刻补回 `WS_CAPTION`，控制栏重新出现 |
| 鼠标再次离开 ≥ 1.2 秒 | 再次隐藏 |

实现要点（全部在窗口自己的线程里做）：

- 子类过程首次收到消息时装一个 150ms 定时器（`TIMER_CAPTION`），在 `WM_TIMER` 里调 `CaptionTick()`。
  **★ 光靠这个不够**：真实 `ScreenRender.exe` 的消息循环取 `WM_TIMER` 的节奏很不规律
  （实测最大化后一直不隐藏，且没有任何 tick）。所以 DLL 另起一个 `CaptionPumpProc`
  心跳线程，每 250ms 给 `BypassUserMax` 窗口 `PostMessage(WM_BYPASS_CAP, 2)` ——
  这是普通队列消息，不依赖定时器合成规则。别的线程只允许 `PostMessage`，
  **绝不跨线程改样式**（见 1.5）。
- `CaptionApply()` 先设 `BypassCapHide` 再改样式：`WM_STYLECHANGED` 与 IAT 的 `SetWindowLong*`
  （`SanitizeStyle`）看到这个属性就不再补回 `WS_CAPTION`，否则刚藏好又被补回来。
- 改样式时带 `SWP_NOSENDCHANGING`：这次 `SWP_FRAMECHANGED` 不经过
  `WM_WINDOWPOSCHANGING`，否则会被"冻结应用几何"那段挡下、边框不重算、客户区不会铺满。
  （属性 `BypassCapApply` 仍保留在代码里，但当前实现不再依赖它。）
- 还原（`SC_RESTORE` / 再点最大化 / 把大窗拖小）都会 `RemoveProp(BypassCapHide)` 并补回标题栏，
  保证小窗能拖、按钮能点。

---

## 2. 配置与安全阀

持久配置在 **DLL 所在目录** 的 `bypass_config.ini`（主程序启动时生成；手工保存后 1 秒内生效）：

| 键 | 作用 |
|---|---|
| `window_mode` | `1` = 窗口模式（默认）；`0` = 关闭（不改 `fullscreen` 参数、不清 `lpDesktop`、不拦 `SwitchDesktop`、不拦全局钩子） |
| `title_autohide` | `1` = 最大化后自动隐藏标题栏（默认）；`0` = 关闭（见 1.8） |
| `minimize_mode` | `taskbar`（默认，真最小化）/ `tray`（隐藏收起，不占任务栏） |
| `small` | 空/`0` = 按源窗口宽高比 + 屏宽 1/3；`0.35` = 屏宽比例；`640x360` = 固定外框尺寸 |

> 升级场景：`bypass_config.ini` 不存在时，主程序会把旧的 `.off` / `.txt` 开关读取一次并写入配置；之后不再使用旧文件。

运行期通过 `Local\BypassTool_State_v1` 共享内存 + `Local\BypassTool_Event_v1` 事件同步：

- 主程序改设置后先原子写配置文件，再发布共享内存并 `SetEvent`；DLL 立即刷新，同时每次刷新也读配置文件（主程序退出后仍可手工改）。
- 「强制小窗」「恢复被收起的窗口」是共享内存里的 `small_seq` / `show_seq` 计数器，不再写时间戳文件。
- 主程序退出后，DLL 继续使用最后一次状态；重开主程序会重新发布。
- 共享内存打不开时（权限/跨用户），DLL 直接读 `bypass_config.ini`。

安全阀仍是单独文件：

| 文件 | 作用 |
|---|---|
| `bypass_hook.off` | 存在 = **放行**：已注入的 DLL 停止干预窗口（渲染进程不用重启）。只在两个时刻写：① 关掉「持续保持小窗」；② 托盘菜单「恢复全屏广播（放行）」。退出程序不写；下次启动会自动清掉上次残留。 |

### 2.1 桌面守护（DLL + 主程序双重）

`MultiClient.exe` 会 `SwitchDesktop` 到 `NetTeachingDesktop`，那之后你的桌面（explorer、
任务栏、其它窗口）根本不在屏幕上 —— 看起来就是"全黑，只剩画面"。

**为什么必须有两层**：如果控制连接断了（教师端异常/实验中断），学生端**不会**自己切回来，
那个桌面没有任务栏、鼠标又被 `WH_MOUSE_LL` 丢弃，表现就是"彻底卡死，只能重启"。
2026-10-22 实测踩过这个坑。

| 层 | 位置 | 频率 | 作用 |
|---|---|---|---|
| DLL 内 | `bypass_hook.c` `EnsureWindowDesktop()` | 2s | **只记录** `desktop mismatch`，不切换（避免两个守护互相对拉） |
| 主程序 | `bypass.c` `DesktopGuardProc()` | 1s（3s 去抖） | 唯一执行切换的一层：切回 `Default`；来自 `MCLOCK Desktop` 时**连带解锁键盘** |

> ⚠ **看门狗只在「持续保持小窗」开启时才工作。**
> 没开小窗模式 = 用户要正常全屏广播，此时看门狗**完全不动**，
> 否则会把广播画面藏到另一个桌面（表现为"透明 + 鼠标点不动"）。
> 热键 `Ctrl+Alt+D` 是显式操作，任何状态下都可用。

主程序这一层不依赖注入是否成功，所以**任何情况下都能把你救回来**。日志：

```
[DesktopGuard] input='NetTeachingDesktop' -> switched to Default (broadcast process still running)
```

热键 `Ctrl+Alt+D` 可立即触发一次；托盘菜单也有「立即切回桌面」。

> **为什么 MCLOCK 要连带解锁**：黑屏锁屏是"切桌面 + 锁键盘"两步。
> 只切回桌面的话，用户看得到自己的桌面却打不了字，比黑屏更迷惑。

### 2.1b 挂起注入（DLL 内）

`MultiClient.exe` 由 Student 收到 `0x2D` 后**毫秒级**拉起，等 5 秒轮询注进去时，
它早就 `SwitchDesktop` 完了。

现在 `CreateProcessA/W` 钩子对白名单进程（MultiClient/ScreenRender/MultiRender/
SharedDesktop/AudioRender）改成：

```
CREATE_SUSPENDED -> 注入 bypass_hook32.dll -> ResumeThread
```

日志：`inject-at-birth pid=...`

### 2.1b2 课堂软件在 bypass 之后启动

原来的轮询是固定 5 秒一次，课堂软件后启动时可能来不及——Student 一起就被老师
下发命令，MultiClient 在钩子到位前就把桌面切了。

现在改成 **"PID 集合变了就立刻注入"**：

| 场景 | 行为 |
|---|---|
| 启动时课堂已在运行 | 启动即注入（不用等） |
| 课堂不在运行 | 每 400ms 扫一次，**不启动 inject32**（只看进程表，几乎零开销） |
| Student 刚出现 | 下一次扫描（≤400ms）发现 PID 变化 → 立刻注入 |
| 之后 MultiClient 被拉起 | 由 Student 里的 `CreateProcess` 钩子**挂起注入**，赶在 `SwitchDesktop` 之前 |
| 一直没新进程 | 每 15 秒复查一次，避免空转 |

> 内存里只保存一个 `TargetPidHash`（目标进程 PID 的哈希）。
> 进程集合一变就注入；没变就 15 秒才复查，不会每秒拉一个 `inject32.exe`。

### 2.2 应急点击（主程序内）

`MultiClient.exe` 的 `WH_MOUSE_LL` 只放行 `dwExtraInfo == 0x12452` 的事件。
`Ctrl+Alt+E` 直接借用这个通行证发一次左键点击，鼠标被冻结时也能点一下。

> 已移除的功能：「改写配置文件」（`core.conf` 三个限制键清零）与「解除黑屏」——
> 实测没用（改回的键/黑屏由课堂软件随时恢复），按钮与自动路径都已删除；
> 「检查当前状态」仍会**只读**显示 core.conf 的键值，便于诊断。

### 2.2b 托盘图标与"最小化后消失"

主窗口带 `WS_EX_APPWINDOW`：**窗口可见时任务栏上有按钮**，最小化 / 关闭 / `Ctrl+Alt+H`
都只是 `ShowWindow(SW_HIDE)`（按钮随隐藏消失，窗口对象必须一直存在 ——
`Shell_NotifyIcon` 的宿主窗口没了，图标也就没了），之后靠托盘图标回来。

> 之前窗口是 `WS_EX_TOOLWINDOW`（任务栏上没有按钮），托盘图标是唯一入口。
> 实测在这台机器上 Windows 10 把图标放进了「`^` 显示隐藏的图标」折叠区
> （可见区 0 个按钮、折叠区 15 个），于是"最小化 = 窗口凭空消失"。
> 现在保留任务栏按钮，折叠区里找不到图标也不会丢窗口。

这里踩过的坑：图标没建出来时窗口照旧 `SW_HIDE`，表现就是"窗口消失、托盘里也没有、
只能靠 `Ctrl+Alt+H` 找回来"。现在的做法：

| 情况 | 处理 |
|---|---|
| `Shell_NotifyIconW(NIM_ADD)` 失败 | 每 500ms 重试，最多 20 次（登录后立刻启动 / shell 还没就绪时 shell 的托盘不可用） |
| Explorer 重启（广播 `TaskbarCreated`） | 置 `g_trayAdded = FALSE` 后重新 `NIM_ADD`，否则重启资源管理器后图标永久消失 |
| 重试全失败 | **拒绝隐藏窗口**（宁可留在屏幕上），日志框给出提示，不把用户困住 |
| 排查信息 | 启动时往 `bypass_hook.log` 写一行 `[Tray] env`：session / 用户 / 窗口站 / 桌面 / 完整性级别 / `Shell_TrayWnd` 句柄 |

`env` 行的意义：进程如果不在**用户会话的 Default 桌面**上，`Shell_NotifyIcon` 会返回成功，
图标却永远不会出现在用户的托盘里；`tray_hwnd=` 为 0 基本就能定位到这一类问题。
（GUI 模式 `AppendLog` 只写编辑框，所以托盘事件另走 `FileLog()` 直接追加到同一个日志文件。）

### 2.3 虚拟桌面固定（把广播窗钉在"一号桌面"）

课堂软件只认 **Win32 桌面**（`NetTeachingDesktop` / `MCLOCK Desktop`），不认 **Windows 虚拟桌面**。
每次开始广播都会新起一个 `ScreenRender.exe`，窗口建在"当时正在看的那块虚拟桌面"上：
学生切到二号桌面后重新广播，窗口就跟到二号桌面，外壳还可能把学生一起拽回去。

做法（`src/bypass_vd.inc`，纯 ASCII，用 `#include` 挂进 `bypass_hook.c`）：

| 时机 | 动作 |
|---|---|
| `CreateWindowExW` 返回后 | 渲染窗（`WS_POPUP`，类 `MultiRenderWindowClass`，实测 `ScreenRender.exe` 0x414325 / 0x4143c9）**此时还不可见**，立刻 `MoveWindowToDesktop` |
| `ShowWindow` / `ShowWindowAsync` / `SetWindowPos(SWP_SHOWWINDOW)` 之前 | 再钉一次（隐藏时 `GetWindowDesktopId` 返回全零，但移动会被记住；首次显示就直接出现在目标桌面） |
| 每 1 秒兜底扫描 | 本进程所有可见渲染窗逐个校验 |
| 目标桌面不是用户正在看的那个 | 拦掉所有**激活**路径（见下表），避免外壳把学生拽过去 |

实现要点：

- COM 只在**私有 STA 线程**上初始化（`CoInitializeEx` + `CoCreateInstance`，
  `CLSID_VirtualDesktopManager {AA509086-5CA9-4C25-8F95-589D3C07B48A}` /
  `IID_IVirtualDesktopManager {A5CD92FF-29BE-454C-8D04-D82879FB3F1B}`）：不在 `DllMain`，也不在渲染器 UI 线程。
  实测窗口所有者线程被阻塞时，从另一 STA 线程调用依然成功。
- 钩子线程把窗口"投递"给 VD 线程并最多等 250ms，保证 `ShowWindow` 返回**之前**窗口已经在目标桌面。
- 目标桌面解析顺序：`guid=` 显式 → `pinned=`（保存的桌面，且仍在注册表列表里）→
  注册表 `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\VirtualDesktops\VirtualDesktopIDs`
  的第 `desktop=` 个 → 兜底采用窗口当前桌面。
- 解析结果写回 `pinned=`：渲染器每次广播都是**新进程**，所以必须落盘，不能只存内存。
- 目标桌面已被关闭（`MoveWindowToDesktop` 返回 `0x8002802B`，TYPE_E_ELEMENTNOTFOUND）时回退到窗口当前桌面。
- 窗口跑在**非输入 Win32 桌面**上时 API 返回 `0x80070005`（E_ACCESSDENIED）：这种进程钉不了，
  打**一条**日志后在本进程内关掉该功能（不再按秒重试、不刷日志）。

#### 激活抑制：为什么"重新广播"不会再把学生拽过去

光把窗口移过去还不够。重新广播时新渲染窗是**别的进程**建的，`MultiClient` 会对它调
`ShowWindow(SW_SHOW)` / `SetForegroundWindow` / `SetWindowPos(..., 0)`，而**激活一个位于
非当前桌面上的窗口**正是外壳切换虚拟桌面的触发条件 —— 于是学生被从二号桌面拽回一号桌面。

判定叫 `g_vdTargetIsCur`：**目标桌面是不是用户当前正在看的那个**。每轮 VD 线程刷新，两个来源都先用
`IsWindowOnCurrentVirtualDesktop()` 校验：

1. **自己的渲染窗**（已钉到目标桌面）—— 直接问"它在不在当前桌面"就是答案；
2. **前台窗口** —— 用 `GetWindowDesktopId()` 拿桌面，再用 `IsWindowOnCurrentVirtualDesktop()`
   确认它确实在当前桌面。

第 2 条必须双重校验，原因是实测出来的：**用户切到一块空桌面时，Windows 会把前台窗口留在原来
那块桌面上**，此时它的 `GetWindowDesktopId()` 仍然等于被钉住的桌面 —— 只看 ID 会得出"用户就在
被钉桌面上"的错误结论，于是放行了新广播的第一次 `ShowWindow(SW_SHOW)`，桌面就被带走了。

另外两条硬规则：

- **只信任"可见窗口"的信号**：隐藏窗口的 `GetWindowDesktopId()` 返回全零、`IsWindowOnCurrentVirtualDesktop()`
  恒为 1（实测，`bin\vd_pin_test.exe --signals`）。所以新进程还没有自己的渲染窗时，两个来源都可能
  答不上来。
- **答不上来时按"不在当前桌面"处理**（`g_vdTargetIsCur` 初值/保持值为 `0`）：新进程刚开始那
  几百毫秒正是广播窗出现、最容易把桌面带走的时刻，宁可少激活一次，也不能赌错。

它为 `0` 时，所有会把渲染窗激活的路径都被改写：

| 调用 | 改写 | 日志 |
|---|---|---|
| `ShowWindow(SW_SHOW / SW_SHOWNORMAL / SW_SHOWDEFAULT / SW_RESTORE / SW_SHOWMAXIMIZED / SW_MAXIMIZE)` | `SW_SHOWNOACTIVATE`（照常显示，不抢前台） | `vd: ShowWindow(5) -> SW_SHOWNOACTIVATE` |
| `ShowWindowAsync(...)` | 同上 | `vd: ShowWindowAsync(5) -> SW_SHOWNOACTIVATE` |
| `SetWindowPos(..., flags 无 SWP_NOACTIVATE)` | 补上 `SWP_NOACTIVATE` | `vd: forced SWP_NOACTIVATE` |
| `SetWindowPlacement(..., showCmd=显示/还原/最大化)` | 把 `showCmd` 改成 `SW_SHOWNOACTIVATE` | `vd: SetWindowPlacement(show=3) -> SW_SHOWNOACTIVATE` |
| `SetForegroundWindow` / `BringWindowToTop` / `SetActiveWindow` | 直接返回成功，不真正激活 | `vd: suppressed SetForegroundWindow/BringWindowToTop/SetActiveWindow` |

几处刻意的选择：

- **按窗口类判断**（`MultiRenderWindowClass`），不要求"这个句柄是我们钉过的那个"：
  激活调用往往来自 `MultiClient.exe`（另一个进程），它看不到我们的内部状态；
  而且窗口模式小窗不走 `IsBroadcastWindow()`，所以 `SetWindowPos` 的补 `SWP_NOACTIVATE`
  和 `SWP_SHOWWINDOW` 的提前钉桌面都放在该判断**之外**（见 `Hook_SetWindowPos` 开头）。
- `g_vdTargetIsCur` 由 VD 线程维护，钩子里只读一个 `volatile LONG`，不做 COM 调用
  （COM 会阻塞渲染线程，见 1.5 节）。新进程第一次投递钉桌面请求时会等 VD 线程初始化完成
  （最多 500ms），避免"第一次 ShowWindow 早于目标解析"。
- 用户本人就在目标桌面时（`g_vdTargetIsCur == 1`）所有改写都不发生：点击、拖动、
  最大化的手感与原生一致。
- 如果 `MoveWindowToDesktop` 时窗口正好是前台窗口，会打一条
  `vd: moving the foreground window ... - the shell may follow it`：外壳确实会跟着这种移动切桌面，
  这条日志用于确认该情况是否还会出现。

> 这条只解决"窗口在别的桌面时不要激活它"。**如果窗口本来就在当前桌面**，
> 激活它就是正常的，工具不会拦。

配置 `bypass_vd.ini`（**DLL 同目录**，首次运行自动生成）：

| 键 | 作用 |
|---|---|
| `enabled` | `1` = 开启钉桌面（默认）；`0` = 老行为（窗口跟着当前桌面走） |
| `desktop` | 虚拟桌面序号，`0` = 第一个（默认） |
| `guid` | 可选，直接写 `{GUID}`，优先于 `desktop=` |
| `pinned` | 工具自己写：**实际生效**的桌面。手工清空 = 下次按 `desktop=` / `guid=` 重新解析 |

配置每 5 秒重读一次，改完不用重启课堂软件（但渲染窗是每次广播新建的，下一次广播才按新值走）。

> ⚠ 限制：只有**建在输入 Win32 桌面（`Default`）上**的渲染窗才能被移动。会话若被切到
> `NetTeachingDesktop`，必须先靠 `CreateProcess*` 钩子清掉 `lpDesktop`、让渲染器建在 `Default`
> （工具已有这段逻辑，日志里是 `desktop=Default`），钉桌面才会生效。

### 2.4 控制端「窗口模式广播」的小窗

老师发广播时如果选的是**窗口模式**（不是全屏），学生机上的渲染窗**出生就是小尺寸 + `WS_EX_TOPMOST`**，根本不经过 1.2 节的降级流程 —— 它没满屏，`IsWorthFixing()` 为假。这类窗口有两个独立缺陷：

| 缺陷 | 后果 |
|---|---|
| enforcer 早退 | `EnforceProc` 里 `if (!IsWorthFixing(h) && !LooksManaged(h)) return TRUE;` 直接跳过它，出生时带的 `WS_EX_TOPMOST` 永远摘不掉：小窗一直压在任务栏和其它应用之上 |
| 转换即抬升 | `Hook_SetWindowPos` 把应用请求的 `HWND_TOPMOST` 改写成 `HWND_NOTOPMOST`。这只清属性，但 `SetWindowPos(HWND_NOTOPMOST)` 本身会把窗口**抬到普通窗口带的最前面**。渲染器每 500ms 重申一次，于是学生每点一下自己的窗口，半秒后又被小窗盖回去 |

修复分四处（都在 `src/bypass_hook.c`）：

| 位置 | 改动 |
|---|---|
| `SmallBcastNoRaise()` | 判定"普通小广播窗"：渲染器类名 + 不是前台窗口 + 未被接管 + 不值得降级 + 当前没有置顶位 |
| `Hook_SetWindowPos` 的 TOPMOST 分支 | 命中上一条时不再改成 `HWND_NOTOPMOST`，而是 `after = NULL; flags \|= SWP_NOZORDER`：**冻结 Z 序**（原样不动），不主动抬升 |
| `EnforceProc` 早退分支 | `!IsWorthFixing && !LooksManaged` 时，若类名是渲染器且仍带 `WS_EX_TOPMOST`，投递 `WM_BYPASS_FIX`(force=2) |
| `FixOnOwnerThread(force==2)` | 在窗口自己的线程里只摘 `WS_EX_TOPMOST` + 一次 `HWND_NOTOPMOST`，**不碰几何**，日志 `untopmost hwnd=... (window-mode broadcast)` |

> ⚠ EnforceProc 只做判断，真正的修改仍然 PostMessage 回窗口线程执行（见 1.5 节）：跨线程 `SetWindowLong` 会和渲染线程互等。

> ⚠ 用户点击/激活小窗时**照常允许抬升**（那时它就是前台窗口，这条 Z 序请求是合法的）；被冻结的只有应用自己发起的置顶/抬升。

自测（`bin\wmode_test.exe`，模拟"出生即小窗 + 置顶 + 每 500ms 重申"，并带一个普通参考窗当"学生正在用的窗口"）：

```bat
rem DLL 的日志和 config 都以 DLL 所在目录为准, 建议复制到临时目录再跑
cd /d %TEMP%
mkdir wmodetest & cd wmodetest
copy /y <classroom-window-helper>\bin\wmode_test.exe .
copy /y <classroom-window-helper>\bin\inject32.exe .
copy /y <classroom-window-helper>\bin\bypass_hook32.dll .
copy /y <classroom-window-helper>\bin\bypass_config.ini .
copy /y <classroom-window-helper>\bin\bypass_vd.ini .
wmode_test.exe --ticks 6 --inject bypass_hook32.dll
```

| 输出 | 含义 |
|---|---|
| `SUMMARY polls=6 top_set=0 above_probe=0` + `VERDICT FIXED` | 通过：置顶被摘、Z 序不再被抬 |
| `top_set=6 above_probe=6` | 没注入（基线） |
| `top_set=0 above_probe=6` | 旧 DLL 的缺陷：属性摘了，但每次重申都抬到参考窗之上 |
| `bypass_hook.off` 存在时 `top_set=6` | 总开关正常，钩子完全不动窗口 |

限制：只有渲染窗在**本进程**（`opid == g_pid`）时才走这条快速路径；跨进程的小窗依赖那个进程已被注入（注入后它自己就成 `g_pid`）。类名不在渲染器白名单（`MultiRender*` / `StreamRenderWindow*` / `OeScreenRenderWindow*` / `SDL_app`）里的窗口一律不碰。

### 2.5 启动时的提权（`src\elevate.c` + `bin\lpe\`）

工具本身要管理员权限：驱动 `IOCTL 0xB2008` 解锁键盘、注入 Student / MultiClient / ScreenRender。
原来的做法只有一条路 —— `IsUserAnAdmin()` 为假就 `ShellExecuteEx("runas")` 弹 UAC。现在多了一条
**自动提权**，并且失败时自动退回到"手动给管理员"的模式：

| 模式（`bin\bypass_elevate.ini` 的 `mode=`，`--elevate=` 覆盖） | 流程 |
|---|---|
| `lpe` / `auto`（默认，前提是 `bin\lpe\trigger.ps1` 存在） | 进度窗 → `bin\lpe\trigger.ps1` → 等日志里的握手行（`elevated instance pid=...`）→ 成功则退出启动器；失败 → UAC；再失败 → 手动指引框（可重试）。**没有确认框**：双击即开始，失败自动回退 |
| `uac` | 只走 UAC（老行为） |
| `manual` | 不自动启动任何东西，只显示手动指引（「重试」仍弹 UAC） |
| `off` | 不处理提权（调试） |

自动提权用的是 **CVE-2026-49176**（WalletService 以 LocalSystem 打开用户重定向的
`Documents\Wallet\wallet.db` 并启用 ESE 持久化回调 → 以 SYSTEM 加载回调 DLL）：

```
bypass.exe (普通用户)
  └─ powershell -EncodedCommand <trigger.ps1 + 参数，无中间文件>
       ├─ wallet_ese_seed.exe     造带回调的 wallet.db（回调 = wallet_callback_shell.dll）
       ├─ documents_path.exe set  把 Documents 指到本次的临时目录
       └─ WalletManager.RequestStoreAsync()  →  WalletService(LocalSystem)
            └─ wallet_callback_shell.dll  →  创建按需服务
                 └─ shell_broker.exe <result.txt>   （本仓库 src\lpe_broker.c 编译）
                      ├─ 读 <result.txt 同目录>\command.txt（UTF-16，内容是 "bin\bypass.exe ..."）
                      ├─ 取交互会话令牌：WTSQueryUserToken → TokenLinkedToken（未过滤令牌，
                      │  CheckTokenMembership 验证 Administrators 成员），失败才退回 SYSTEM 令牌
                      └─ CreateProcessAsUserW(..., winsta0\default) → 新的 bypass.exe
```

几个刻意的设计：

几个刻意的设计：

- **已在运行就不再折腾**：单实例检查放在**提权之前**——第二次双击时探测
  `ClassroomBypassTool_SingleInstance` 互斥体，已存在就直接唤醒旧窗口退出
  （通过窗口放行的 `WM_BYPASS_SHOW` 消息，低权限实例也能唤醒已提权实例），
  不会白跑 LPE/UAC。提权链路自己拉起的实例（命令行带 `--elevate=uac`）跳过这个
  探测，由它们在提权成功后持有互斥体。
- **不经过脚本执行策略**：脚本内容是读出来、拼上参数、Base64 成 UTF-16LE 后交给
  `powershell -EncodedCommand` 的。执行策略（包括被组策略锁死的 `Restricted`）只约束
  `*.ps1` **文件**，`-EncodedCommand` 直接执行代码，所以“禁止运行脚本”的机器也能走这条路。
  （手工跑才需要 `-ExecutionPolicy Bypass -File`。）
- **`result.txt` 只允许 ASCII**：回调 DLL（原样二进制）和 `shell_broker.exe` 都往同一个文件追加
  事件行，读取方（`trigger.ps1`）用默认编码 + 正则匹配。任何一边写 UTF-16 都会让两种编码混进
  一个文件，匹配永远失败（第一版集成就是这么挂的：broker 用宽字符写事件 → 脚本读不出来 →
  超时报"No shell result was received"）。唯一的 UTF-16 文件是 `command.txt`，它只被写它的
  broker 自己读。broker 还会写 `note=...` 行（每一步失败时记下是哪个 API、什么错误码），
  触发脚本不匹配它们，但提权失败时这些行会出现在 `bypass_hook.log` 的 payload 输出段里。
- **不传 `lpCurrentDirectory`**：第一版集成里 broker 创建进程报 `ERROR_DIRECTORY (267)`，
  源头就是从命令行解析出的工作目录参数。`bypass.exe` 的所有路径都从自己的 exe 位置推导，
  完全不需要工作目录 —— 现在传 `NULL`（子进程继承服务的 System32），参数直接消失。
- **"user-high"令牌的坑（实测踩过）**：`WTSQueryUserToken` 给的是会话的 **UAC 过滤令牌**——
  完整性能提到 High，但 `Administrators` 组仍是 deny-only，`IsUserAnAdmin()` 为假。第一次集成
  就是这样：broker "成功"拉起了子进程，子进程却不是管理员，只能再弹一次 UAC。现在 broker 用
  `GetTokenInformation(TokenLinkedToken)` 取**未过滤令牌**（SYSTEM 有 SeTcbPrivilege 才允许），
  并用 `CheckTokenMembership` 验证真的在 Administrators 组里才拉起；拿不到就退回 SYSTEM 路径。
- **启动器的等待窗口**：脚本 `exit=0` 表示链路成功、子进程可能正在跑它自己的 UAC 步骤（要人点），
  所以成功场景等 25 秒、失败场景 2.5 秒就走；等待期间还会兜底解析共享日志里的
  `child_pid=`（进程活着就认账）。子进程彻底放弃（用户取消了 UAC）时在日志里写
  `ELEVATE_ABANDON pid=...`，启动器立即改走手动提示，不会再弹第二个 UAC。
- **只信任握手行，不信任退出码**：`wallet_callback_shell.dll` 只能证明"服务被拉起来了"，
  真正要确认的是"我们的程序以管理员身份跑起来了"。所以被提升的实例启动后第一件事就是往
  `bypass_hook.log` 写一行 `elevated instance pid=... is up`，启动器扫日志尾部找到这行并确认
  进程还在（高完整性进程打不开时按"活着"处理），才算成功。
- **等待期间一定有反馈**：启动器在自己线程里开一个置顶小窗（不可激活、不进任务栏），
  显示当前阶段，并提供「跳过，直接弹 UAC」按钮；脚本每走一步往共享日志写一行 `[elevate] ...`，
  PowerShell 的 stdout/stderr 也直接流进同一个文件——失败原因不用再翻别的文件。
- **优先"普通用户 + 未过滤令牌"而不是 SYSTEM**：托盘、主题、用户配置目录都是用户自己的；
  只有 `TokenLinkedToken` 拿不到或令牌不在 Administrators 组时才退回 SYSTEM
  （这就是 PoC 原本的行为）。
- **失败一定落到 UAC/手动**：补丁机、Defender 拦截、超时（60 秒）、用户点「跳过」，全部走同一条
  回退路径，不会出现"点了没反应"。
- **`Documents` 一定还原**：`trigger.ps1` 的 `finally` 无条件恢复原路径，否则学生机的"文档"
  会被指到临时目录。
- **只有一个日志文件**：提权过程、payload 输出、脚本进度全部进 `bin\bypass_hook.log`
  （启动器先截断；被 broker 拉起的实例**不截断**——它们的历史记录和握手行都在里面，启动器
  还要扫它们）。日志用 `FILE_APPEND_DATA` 打开，多进程并发追加各自落到真实文件尾。

调试用：`bin\bypass.exe --elevate-selftest` 只把编码后的命令写到
`%TEMP%\bypass_elevate_selftest.b64`（可与 PowerShell 自己算出的 Base64 逐字节比对），
**不会启动任何东西**。

`lpe\payload\` 里除 `shell_broker.exe` 外的三个文件是 PoC 原样二进制（见 `lpe\payload\README.txt`
的来源与 SHA256）；`shell_broker.exe` 由 `src\lpe_broker.c` 编译，因为原版把子进程写死成
`cmd.exe`，而这里必须拉起 `bin\bypass.exe`。

---

## 3. 日志与排错

日志文件：

- `bypass_hook.log` —— **唯一的日志文件**。注入的 DLL（每个被注入进程一份输出，行首格式如下：
  `[HH:MM:SS.mmm] [pid N] 内容`，时间戳用于判断"最大化→还原"隔了多久、定时器有没有在跑）和
  主程序的启动/提权过程（`launcher ...`、`lpe: ...`、`[elevate] ...`、
  `elevated instance pid=... is up`）都写在这里。启动器每次启动截断一次；被 broker 拉起的
  实例不截断（链路日志要连续）。所有写方都用追加语义，多进程并发安全。
- 主程序界面日志 —— 按钮操作、扫描结果、被降级的窗口、注入器的 stdout
  （**不再生成 `bypass_inject.log`**：注入器输出走匿名管道直接进界面/日志）

常见关键字：

> 改写类日志（`ShowWindow(5) -> SW_SHOWNOACTIVATE`、`forced SWP_NOACTIVATE`、
> `suppressed ...`、`user left/back`）做了限流：同一个（窗口, 动作）15 秒内只打一条，
> 状态切换 3 秒内只打一条。渲染器每秒会调十几次，不这样会把整份日志淹掉。

| 关键字 | 含义 |
|---|---|
| `attached: <exe> - IAT patched (N entries)` | 注入成功，共 patch 了 N 个导入项 |
| `broadcast_proc=1/0` | 本进程是否被判定为"全屏广播渲染器" |
| `fix hwnd=... 1918x964` | 首次降级某个窗口 |
| `-> now 868x467 (wanted 868x467)` | 降级后回读，尺寸对得上 |
| `!! readback mismatch` | 改完回读不一致 → 另有东西在改这个窗口 |
| `keep user geometry ... (app wanted ...)` | 应用想改尺寸/位置，被冻结 |
| `ignore app window-state change ... (cmd=9)` | 应用想还原/最大化，被忽略（9 = SW_RESTORE） |
| `user restore ... (done by us)` | 用户点还原，由我们摆回用户矩形 |
| `user maximize ... (done by us)` | 用户点最大化，由我们执行 |
| `user-resized hwnd=...` | 用户拖完，记录为新的"用户矩形" |
| `blocked SwitchDesktop(...)` | 拦住了切桌面 |
| `suppressed SetWindowsHookExW(id=14)` | 拦住了全局鼠标钩子（14 = WH_MOUSE_LL） |
| `untopmost hwnd=... (window-mode broadcast)` | 窗口模式广播小窗出生时带的 `WS_EX_TOPMOST` 已摘掉（见 2.4）；正常只在出生后出现一次 |
| `STUCK ... owner hook missing?` | 某个窗口所在的进程还没被注入（跨进程兜底用尽） |
| `caption timer armed` / `caption timer first tick` | 标题栏定时器装上 / 第一次触发。只有 armed 没有 first tick → 渲染器不取 `WM_TIMER`，由 250ms 心跳兜底 |
| `caption tick hwnd=... pt=... band=...` | 每秒一行的判定现场：光标在不在标题栏带里、`idle` 多久了 |
| `caption hide due` → `caption auto-hide` | 1.2 秒到了，真正摘掉 `WS_CAPTION` |
| `caption tick skip (...)` | 被开关/接管状态挡住（括号里直接给出 disabled/autohide/managed/iconic/visible） |
| `vd: target = desktop[N] = {...} (M desktop(s) in registry)` | 虚拟桌面固定：解析出的目标桌面（只在广播渲染进程里打） |
| `vd: pinning on desktop=N target={GUID} targetIsCurrent=N (activation suppressed/allowed)` | 本进程的钉桌面状态；`suppressed` = 你在别的桌面，随后的激活请求都会被改写。每个进程启动时只打一行 |
| `vd: pinned hwnd=... -> {...} (targetIsCurrent=0 visible=1 wasHiddenId=0)` | 渲染窗成功钉到目标桌面；`targetIsCurrent=0` = 目标桌面不是用户当前在看的那个，激活抑制生效 |
| `vd: user left the pinned desktop - render window activation suppressed` | 用户切到别的桌面，从这里开始拦激活（切回时打 `vd: user is back on the pinned desktop`） |
| `vd: ShowWindow(5) -> SW_SHOWNOACTIVATE` / `vd: ShowWindowAsync(5) -> ...` | 应用想显示并激活渲染窗，被改成"只显示不激活"（5 = SW_SHOW） |
| `vd: forced SWP_NOACTIVATE hwnd=...` | `SetWindowPos` 没带 `SWP_NOACTIVATE`，被补上 |
| `vd: SetWindowPlacement(show=N) -> SW_SHOWNOACTIVATE` | 应用用 `SetWindowPlacement` 显示/还原窗口，改成不激活 |
| `vd: suppressed SetForegroundWindow/BringWindowToTop/SetActiveWindow(...)` | 目标桌面不是当前桌面，拦掉前台/置顶请求 |
| `vd: moving the foreground window ... - the shell may follow it` | 钉桌面时窗口恰好是前台窗口（外壳会跟着它切桌面）；正常情况下不应出现 |
| `vd: pinning disabled - ... E_ACCESSDENIED` | 进程在非输入 Win32 桌面上，虚拟桌面 API 拒绝操作；本进程内已关闭该功能（只打一条） |
| `vd: target desktop no longer exists - fallback to {...}` | 目标虚拟桌面被关掉了，回退到窗口当前桌面 |

排错顺序建议：
1. 看 `build=` 是不是最新构建；
2. 看 `window_mode=` 是不是 `on`；
3. 看 `broadcast_proc=1` 有没有出现（没有就说明身份判定没命中，走的是尺寸兜底）；
4. 看 `keep user geometry` / `ignore app window-state change` 是否成对出现（成对 = 正常对抗）。

---

## 4. 自测（不需要安装课堂软件）

`bin\hooktest32.exe` 是一个模拟渲染器：

- 主窗口类名 `MultiRenderWindowClass`，尺寸 3/4 屏；
- 每 500ms 做一套和真实渲染器一样的动作：
  `ShowWindow(SW_RESTORE)` → `SetWindowLong(GWL_STYLE, 去掉 WS_MAXIMIZE)` →
  `SetWindowPos(小矩形, SWP_FRAMECHANGED)`并在循环里把窗口改成全屏 + 最大化；
- 额外开两个"陪测窗口"：
  - `HookTestVDesk`（75% 屏 + 置顶 + 带框）= 虚拟桌面预览窗，**必须保持原样**；
  - `StudentHelperWnd.oseasy`（满屏）= 学生端界面，**必须保持原样**。

跑法（管理员 CMD）：

```bat
cd /d <classroom-window-helper>
start "" bin\hooktest32.exe "{#decoderName#:#h264#,#fullscreen#:1}"
timeout /t 2 >nul
bin\inject32.exe "%CD%\bin\bypass_hook32.dll"
```

> DLL 在本地固定盘时直接注入，**不复制到 `%TEMP%`**；只有共享目录 / 网络盘 / 可移动盘才走 `bypass_stage` 转存。

预期：主窗口先被降级成小窗（约 869x518），之后能最大化/还原/拖动；
另外两个窗口尺寸全程不变；日志里只有一条 `subclassed`（主窗口）。

> `inject32.exe` 的目标进程列表里包含 `hooktest32.exe`，仅为自测方便。

验证"最大化后标题栏自动隐藏"（同样的自测窗口）：

1. 注入后给主窗口补一个 `BypassSmall` 属性（自测渲染器会一直把 `WS_MAXIMIZE`
   置上，DLL 的 enforcer 因此把它当"系统最大化"而跳过 `MarkManaged`；真实
   `ScreenRender.exe` 不会这样）。
2. 给窗口发 `WM_SYSCOMMAND / SC_MAXIMIZE`（模拟点最大化）。
3. 光标移开 1.2 秒 → 样式里 `WS_CAPTION` 消失；把光标移到窗口顶边 6px 内 →
   `WS_CAPTION` 立刻回来。

日志出现 `caption auto-hide ... (maximized broadcast)` 与
`caption auto-show ... (control bar visible)` 即生效。

验证"虚拟桌面固定"（两种办法，两个虚拟桌面即可）：

**A. 自动回归 `bin\vd_pin_test.exe`**（`make.bat vdtest`）

```bat
rem 目标桌面必须不是当前桌面, 否则用例会直接退出并提示
rem bypass_vd.ini: enabled=1 / desktop=1   (当前在看 0 号桌面时)
bin\vd_pin_test.exe
```

它把 `bypass_hook32.dll` 直接 `LoadLibrary` 进自己进程，建一个
`MultiRenderWindowClass` 窗口，等 DLL 把它钉到目标桌面，然后依次调用渲染器用到的激活路径
（`SetWindowPos(HWND_TOPMOST, 0)`、`ShowWindow(SW_SHOW)`、`SetForegroundWindow`、
`BringWindowToTop`）。每次调用后回读**前台窗口**所在桌面：桌面变了 / 前台被抢走 = FAIL。

`--signals` 不是回归检查，而是**测 API 本身**：打印 `GetWindowDesktopId` +
`IsWindowOnCurrentVirtualDesktop` 对"隐藏窗 / 可见窗在别的桌面 / Progman / 托盘窗"分别返回什么。
激活抑制的判定规则就是靠它定的（隐藏窗两个 API 都不可信，见 2.3 节）。

`--baseline` 不加载 DLL，自己用 `MoveWindowToDesktop` 把窗口挪到另一桌面再跑同样的调用，
用来确认"这套调用本来就该把桌面带过去"。**注意**：本机 Windows 通常拒绝一个非前台进程
抢前台，所以 baseline 里看到 "refused" 是正常现象 —— 它不能反证注定失败，真正的证据是
日志里的 `vd:` 行（钩子确实被咨询过）。

**B. 手工验证**

1. 在 DLL 同目录的 `bypass_vd.ini` 里把目标写成 0 号桌面：

   ```ini
   enabled=1
   desktop=0
   ```

2. 注入后切到**另一个**虚拟桌面（`Win+Ctrl+→`），再让自测窗口重新 `ShowWindow` / 重新广播。

日志里出现 `vd: pinning on desktop=0 target={...} targetIsCurrent=0 (activation suppressed)`
和 `vd: pinned hwnd=... -> {...} (targetIsCurrent=0 ...)` = 窗口已经在另一块桌面上，
当前桌面看不到它。切到另一桌面后 `vd: user left the pinned desktop` 出现，随后应用试图
激活渲染窗时会打一条改写日志（`vd: forced SWP_NOACTIVATE` / `vd: ShowWindow(5) ->
SW_SHOWNOACTIVATE` / `vd: suppressed ...`，同一窗口同一动作 15 秒最多一条），
而**桌面本身不动**。把 `enabled=0` 写回文件（5 秒内重读）即恢复老行为。

> 手工验证：用 `Win+Ctrl+←/→` 在两块桌面之间切换，窗口应该只在 0 号桌面出现；
> 每次重新广播后当前桌面不应被切走。

---

## 5. 已知限制 / 注意事项

1. **Defender**：DLL 含 IAT patch 与进程注入，容易被判 HackTool。必须加排除项，否则
   新构建会被直接删掉（现象：点按钮报"找不到 DLL"）。
2. **同路径 DLL 不会重跑 `DllMain`**：换构建必须重启课堂软件。
3. **独占全屏（DXGI flip model）**：少数渲染器会用真独占全屏，窗口层手段无效。
   工具会给这些 exe 预设"禁用全屏优化"（`AppCompatFlags\Layers`），**下次启动**才生效。
4. **教师端真正下发"窗口模式广播"时**：应用自己给的就是普通窗口，工具不会再去改造它
   （身份判定 + 90% 几何兜底都会放行）。
5. **多显示器**：几何判定用的是主屏尺寸（`SM_CXSCREEN`），副屏上的广播窗按主屏比例计算。
6. **最小化状态**：最小化时 `GetWindowRect` 返回 `(-32000,-32000)` 幽灵矩形，代码里
   已对 `IsIconic` 做保护，不要在这些路径上取值。
7. **日志体积**：每类事件都有独立计数上限（40/24/20 条），不会无限增长；但 `geo` 行
   在对抗激烈时仍可能较多，属正常。
8. **虚拟桌面固定只在输入 Win32 桌面上有效**：渲染窗若在 `NetTeachingDesktop` 上，
   `MoveWindowToDesktop` 返回 `0x80070005`，日志只留一条 `vd: pinning disabled - ...`；
   此时要靠 `CreateProcess*` 钩子让渲染器建在 `Default` 上（默认已开）。
9. **窗口已在当前桌面时不会"抢回"**：钉桌面只负责把新窗口放到目标桌面；
   用户自己把窗口拖到别的虚拟桌面（Win+Shift+←/→）后，兜底扫描会再把它挪回来。

---


---

## 6. 免责声明

本工具仅用于**自己拥有/被授权管理**的机房与教学环境，用于改善学生端可操作性。
请遵守所在学校/单位的使用规定与当地法律。