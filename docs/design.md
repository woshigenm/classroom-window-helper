# 设计与实现（v2.0）

> 本文说明「小窗化广播」的核心机制、Loader/Core 分层与热重载、开关、日志、自测和已知限制。
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
| `BypassUserCmd` | `WM_SYSCOMMAND`(SC_RESTORE/MAXIMIZE/MINIMIZE/MOVE/SIZE)、`WM_NCLBUTTONDBLCLK` | 用户刚下过命令（**只是前提条件**，几何还要过下面的"== 用户矩形"校验） |
| `BypassUserRc` | 降级完成时 / 每次 `WM_EXITSIZEMOVE` | "用户矩形"：还原的目标，不会被应用缓存尺寸污染；**拖到接近满屏也算**（只有"模拟最大化"状态下的拖动例外） |
| `BypassZoomed` | `WM_WINDOWPOSCHANGED` | 记录当前是否最大化 |

规则：

- **用户拖动**（`BypassSizing`）→ 不插手任何东西。
- **应用发起的几何变更**（`SetWindowPos` / `MoveWindow` / `WM_WINDOWPOSCHANGING`）→
  统一改成 `SWP_NOSIZE | SWP_NOMOVE`（并吞掉 `SWP_FRAMECHANGED`，否则边框重算会把
  `WS_MAXIMIZE` 丢掉）。
- **"用户刚下过命令"不等于"放行应用的几何请求"**：应用每 500ms 会把出生时缓存的
  小矩形再发一遍。早年这里只看"命令是不是 1.5 秒内的"，于是用户一拖边框
  （`SC_SIZE` 会写 `BypassUserCmd`），紧接着那次定时器就被当成"用户还原"放行，
  窗口当场缩回右下角（19:18 实测日志：`syscmd cmd=0xF000 user=1` → 300ms 后
  `geo … -> 441x435@(811,728)`）。现在放行条件是**请求矩形 ≈ 记录的
  `BypassUserRc`（±8px）**；我们自己在 `SC_RESTORE` 里摆回用户矩形正好满足它。
- **应用发起的窗口状态变更**（`ShowWindow` / `ShowWindowAsync` 的
  `SW_RESTORE/SW_SHOWNORMAL/SW_MAXIMIZE/SW_SHOWMAXIMIZED`）→ **一律忽略**。
- **用户的最大化/还原** → 由我们在窗口线程里直接执行：
  - `SC_MAXIMIZE` / 双击标题栏 → **模拟最大化**：不设 `WS_MAXIMIZE`，把外框铺到
    `rcMonitor` 外一圈（边框推到屏幕外，客户区正好铺满整屏），并打 `BypassUserMax`；
  - 再点一次 / `SC_RESTORE` → 摘掉 `BypassUserMax`、补回标题栏，并
    `SetWindowPos(用户矩形)`。
  这样即使应用把 `WS_MAXIMIZE` 位改乱，用户也总能把窗口摆回他要的样子。

实现细节（并发相关，改代码前必读）：

- `BypassUserRc`／几何对照表是**进程内两张表**，由 enforcer 线程和各窗口线程
  并发读写，统一用 `g_geoSs` 保护。`RECT` 是 4 个 `LONG`，不加锁会读到
  "新 left + 旧 right"的撕裂矩形，再拿去 `SetWindowPos` 就是"闪一下、位置错乱"。
- 表满时**轮转覆盖**而不是丢弃：`Student.exe` 的顶层窗口一多，广播窗否则永远
  拿不到槽 → 没有"用户矩形" → 还原/最大化静默失效（日志里什么都不会留下）。
- 跨进程兜底（同族进程还没被注入时）**只做异步几何**：`SetWindowPos` 带
  `SWP_ASYNCWINDOWPOS`，一个样式字节都不写。同步跨进程改窗口的代价实测过：
  对端 UI 线程不泵消息时，`SetWindowPos` **卡了约 64 秒**才返回（`SetWindowLong`
  则直接失败，err=1400），而我们只有一个 enforcer 线程 —— 卡一次就全停。
  异步化的代价是**不能回读校验**（请求还在对端队列里），所以这里没有
  `-> now ... (wanted ...)` 那类校验；是否改成只能看下一轮枚举或对端注入后的日志。
  异步调用失败（对端彻底不响应）时**不**标记"已接管"，下一轮还能再试。
- 内联钩子优先用微软 hotpatch 位（入口前 5 字节 `CC/90` 填充 + 入口 `8B FF`）：
  入口只落一条**对齐的 2 字节** `jmp short`，不存在"半个 `E9 rel32`"的窗口。
  序言不是这个形状时才退回"搬 5 字节到 trampoline"：那条路会**先挂起本进程
  所有其他线程**，确认没有线程的 EIP 停在 `[entry, entry+5)`，再写 5 字节、
  `FlushInstructionCache`、恢复线程（最多重试 3 次；实在打不上就只保留 IAT 钩子
  并记一行 `IAT only (entry patch skipped)`）。渲染器有 500ms 定时器线程在调
  `ShowWindow`，不挂线程直接写 5 字节就是整进程崩。
- `BypassSmall`（"已接管"标记）只认**窗口属性**，不再用进程内表兜底：
  `IsWindow()` 只能证明句柄活着，HWND 数值会被系统复用（`ScreenRender` 重建
  渲染窗就是这种情况），表里的陈旧记录会让新窗口被当成"已接管"而永不降级。
- 子类化判定同理，看**窗口当前的过程指针**（`GWLP_WNDPROC == SubclassProc`），
  不能信进程内表；窗口销毁时 `ForgetWindowGeom()` 会把用户矩形 / 几何 /
  cross 预算几处状态一并清掉。

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

| 模式（由代码决定：`bin\lpe\trigger.ps1` 存在 → `lpe`，否则 `uac`；`--elevate=` 覆盖） | 行为 |
|---|---|
| `lpe`（默认，前提是 `bin\lpe\trigger.ps1` 存在） | 进度窗 → `bin\lpe\trigger.ps1` → 等日志里的握手行（`elevated instance pid=...`）→ 成功则退出启动器；失败 → UAC；再失败 → 手动指引框（可重试）。**没有确认框**：双击即开始，失败自动回退 |
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

### 2.6 屏幕监控守卫（老师端只能看到定格画面）

**需求**：老师端"屏幕监控 / 录屏"时，不要把我们**实时**的桌面传上去。

**实证（REA / Ghidra，`StudentLogic.dll` sha256 `404b4d…9d84`）**：学生端的屏幕抓取只有一条
GDI 路径：

```
FUN_10025610 (0x10025610, 调用方 FUN_10025720)
    hdc = CreateDCW(L"DISPLAY", 0, 0, 0)
    mem = CreateCompatibleDC(hdc); bm = CreateCompatibleBitmap(hdc, w, h)
    BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY | CAPTUREBLT)
    GetDIBits(mem, bm, 0, h, bits, &bmi, 0)        // 之后交给上传/编码
```

所以拦截点只有一个：**源 DC 是屏幕 DC 的 `BitBlt`**（`src\bypass_screen.inc`）。

| 环节 | 做法 |
|---|---|
| 识别屏幕 DC | 记录 `CreateDC(DISPLAY)` / `GetDC(NULL)` / `GetWindowDC(NULL)` 返回的句柄；另一个兜底判据是"当前位图是 1×1 的屏幕位图"（应用自己画过的内存 DC 是真实尺寸，不会误判） |
| `freeze`（默认） | **第一次**抓屏照常读屏幕，把结果存成"定格帧"；之后每次抓屏都 `SetDIBitsToDevice` 从缓存画到调用方的位图里，**屏幕一个像素都不再读**，连 `GetDIBits` 拿到的都是缓存帧 |
| `blank` | 连第一帧都不抓，直接铺一块纯色 |
| 其它 rop | 只有 `SRCCOPY` / `SRCCOPY\|CAPTUREBLT` 才拦（AND/OR/XOR 不是抓屏） |
| 开关 | `bin\bypass_screen.ini`：`enabled=0/1` + `mode=freeze\|blank`，DLL 每 0.5 秒重读一次，改完不用重启；主界面第 3 行的「屏蔽屏幕监控」按钮就是写这个文件（并用 `RunInjector` 保证 DLL 已经进了 `Student.exe`），`Ctrl+Alt+M` 是同一个开关的**备用入口**（面板收进托盘/点不到时用） |

**边界（务必知道）**：

- 只在**注入了 `bypass_hook32.dll` 的进程**里生效（`Student.exe` / `ScreenRecord.exe` 等，
  也就是 `inject32.exe` 的目标列表）；DLL 没进去 = 完全没保护。
- 只覆盖 **GDI** 抓屏。如果哪天厂商换成 DXGI Desktop Duplication / `PrintWindow` 抓屏，
  这里拦不到（`PrintWindow` 目前没钩）。
- 定格帧是"开始监控那一刻"的桌面，之后老师看到的画面**完全不动**——这是"画面固定"的代价，
  正常上课时老师很快能看出来静态。想更像真的可以后续加 `refresh=N`（每 N 秒悄悄换一帧），
  代码里预留了 `g_scSeededTick` 但还没实现。
- 守卫不改变课堂软件的任何协议/传输：上传照常发生，只是内容变成同一张图。

---

### 2.7 屏蔽控制端重启/关机（吞掉 0x27 / 0x28）

**需求**：老师端点「远程关机 / 远程重启」时，本机不执行。

**先确认过反向不可行**（`Teacher.exe` sha256 `8432e1…1e0f`）：老师端只有**发命令**的代码
（`CRemoteShutDownDlg`、`FUN_00497940` 里逐台学生写 `cmd=0x27/0x28`），
导入表没有 `ExitWindowsEx` / `InitiateSystemShutdown*`，也没有 `shutdown -s -t 1`、
`systemoper`、`SYS_RunCommand` 这些字符串，更没有学生端那套 `CShutDownDlg` /
`StuShutDownWnd.xml`。所以把同样的包发回老师端不会执行任何东西；能关的只有学生端自己。

**学生端的执行链（REA / Ghidra，`Student.exe` sha256 `fcb7ab…473f`）**：

```
FUN_00413560(cmd)
  case 0x27 (重启): FUN_00413080 -> FUN_00403a20
        AdjustTokenPrivileges(SeShutdownPrivilege)
        ExitWindowsEx(EWX_REBOOT|EWX_FORCE, 0)          // 直接调用
  case 0x28 (关机): FUN_00412d80 (CShutDownDlg) -> FUN_00412d40
        FUN_00404970(L"shutdown -s -t 1", 0)
          -> LoadLibrary("systemoper.dll") + GetProcAddress("SYS_RunCommand")   // 运行时解析
             -> CreateProcess("shutdown -s -t 1")
```

`systemoper.dll` 是**第一次用到才加载**的，如果只在模块加载后扫 IAT，会正好错过它第一次
被调用的时刻。所以 `src\bypass_shut.inc` 用的是**函数入口 inline hook**（进程内所有模块共享）：

| 拦截点 | 覆盖 | 备注 |
|---|---|---|
| `ExitWindowsEx`（user32） | 0x27 重启 | 直接调用，入口 hook 稳 |
| `CreateProcessA/W`、`WinExec`、`ShellExecuteA/W` | 0x28 的 `shutdown -s -t 1` | 命令行按"可执行名 + `/s` `/r` `/p` `/g`"识别，兼容 `cmd /c`、带引号全路径 |
| `InitiateSystemShutdownExW/A`、`InitiateSystemShutdownW/A` | 兜底 | 客户端没用到，顺手覆盖 |

- 被拦的调用返回**成功**（`ExitWindowsEx` → `TRUE`，`WinExec` → `33`，`ShellExecute` → `(HINSTANCE)33`，
  `CreateProcess` → `TRUE` + 全零 `PROCESS_INFORMATION`），控制端不会把它当成失败重试。
- 开关：`bin\bypass_shut.ini` 的 `enabled=0/1`，DLL 每 0.5 秒重读；主界面勾选框
  「屏蔽控制端重启/关机」和托盘右键菜单里的同名项是**同一个**开关（都走 `SetShutGuard`，
  保证 ini / DLL / 勾选框状态一致），打开时调 `RunInjector` 保证 DLL 已进入 `Student.exe`。
- 只影响被注入的课堂进程；用户自己从开始菜单/`shutdown` 关机不经过这些进程，不受影响。
- 日志（刻意压量，控制端会每 ~20 秒重发）：
  - P1 起入口 hook 由 Loader 装：每个进程一行
    `[Loader] inline hooks: all installed`（失败是 `PARTIAL` / 逐条 `postponed`），
    Core 加载成功是 `[Loader] core loaded: ... build=...`。
  - 拦截前 3 次逐条：`[Guard] blocked ExitWindowsEx flags=0x6 (restart)` /
    `[Guard] blocked CreateProcessW (shutdown -s -t 1)`；之后每 25 次一行
    `[Guard] blocked 25 remote power commands (last: ...)`。
  - 开关状态只在**变化**时记一行；默认关闭不再为每个进程打印 "off"。

- 0x28 的**倒计时窗**：`CShutDownDlg` 的窗口类名就是 `ShutDownDlg`
  （`Student.exe` 的 `FUN_0040d560` 直接 `return L"ShutDownDlg"`；`StuShutDownWnd.xml`
  只是 DuiLib 皮肤）。`CreateWindowExW` 钩子在窗口建出来后立刻 `ShowWindow(SW_HIDE)`，
  不会再出现"系统将在 N 秒后关闭"。故意**不**投递 `WM_CLOSE`：DuiLib 的
  `WindowImplBase::OnClose` 常常直接 `PostQuitMessage(0)`，非模态场景会把 `Student.exe`
  的整个消息循环结束掉；隐藏就够了——倒计时走完仍会调 `SYS_RunCommand`，而那条路已经被
  `CreateProcess` 钩子拦下。

- **版本自检**：DLL 在每个进程写一行
  `[build] image=Student.exe dll=bypass_hook32.dll built=Nov 16 2026 21:08:38 pid=6116`。
  主程序判断"进程里的是不是新 DLL"**不靠这行日志**，而是比较**磁盘 DLL** 与**进程内
  DLL** 的 PE `FileHeader.TimeDateStamp`：
  - `bypass_hook.log` 由 DLL 以 `FILE_APPEND_DATA` 追加，只有 `bypass.exe` 启动时清一次；
    删掉文件后下一次 `HookLog` 会重建，不能当版本台账；
  - `bypass.exe` 和 `bypass_hook32.dll` 是两次编译，`__TIME__` 天然差几秒，字符串比对会误报；
  - PE 头不受日志/共享内存/版本号影响，还能一次看到 `Student.exe`、`MultiClient.exe`、
    `ScreenRecord.exe`、`ScreenRender.exe` 各自加载的是哪个版本。
  启动 4 秒后自动查一次（不一致就警告），「检查当前状态」里也会逐进程列出构建时间。

> **自测注意（血泪教训）**：`tests\shutguard_test.c` **不允许**调用
> `ExitWindowsEx`（实测 `ExitWindowsEx(0x80000000, 0)` 会被 Windows 当成关机，直接把机器关掉），
> 也不允许出现裸的 `shutdown -s -t 1`（会解析到 `System32\shutdown.exe`）。
> 测试只跑 `%TEMP%\shutguard_probe\shutdown.exe`（`cmd.exe` 的重名副本）+ 从日志读
> `[Loader] inline hooks: all installed` / `[Loader] core loaded:`；
> `tests\shutguard_test.ps1` 在运行前会扫描二进制，发现这两类东西就拒绝执行。

### 2.8 Loader / Core 分层（P1）

目的：**改钩子逻辑不用重启课堂客户端**。直接 `FreeLibrary` 旧 DLL 是不行的——Worker
线程、inline hook 的 jmp 目标、IAT 槽、窗口子类化都还指着它的代码段。所以拆成两层：

```
bypass_hook32.dll (Loader, 进程生命周期内不变)
  ├─ DllMain → Manager 线程
  ├─ 解析 real_* → 装 8 个 inline hook（入口 jmp 指向 L_* dispatcher）
  ├─ PatchAllModules()  → 所有模块的 IAT 指向 L_* dispatcher
  ├─ LoadLibrary(bypass_hook_core.dll) → Core_Init(&host, &api)
  └─ g_table = &api  （原子发布；NULL = 直通）
bypass_hook_core.dll (Core)
  ├─ Core_Init() 填 HrCoreApi.hooks（33 个 Hook_*）+ build_id
  ├─ Worker 线程、窗口/抓屏/关机守卫
  └─ Hook_* 只通过 HrHost.real.* 调原函数（内联入口给的是 trampoline）
```

关键点：

- **dispatcher 必须有 NULL 直通**：Core 不在/还在加载时 `g_table == NULL`，
  `L_*` 直接调 `real_*`。这是 P2 卸载窗口期不崩的前提。
- **谁装钩子谁拥有代码**：inline hook 和 IAT 槽永远指向 Loader，Core 卸载后
  jmp 目标仍然有效——所以 **加新拦截点必须重编 Loader**；只改逻辑才只换 Core。
- **real_* 不能再用 GetProcAddress**：入口已经被 Loader 打了 jmp，必须用
  `HrHost.real.*`（trampoline），否则递归/崩溃。
- **状态/日志/路径归 Loader**：日志句柄、`bin\` 前缀、Loader 完整路径都在
  `HrHost` 里传给 Core；Core 不再用 `GetModuleFileName(g_self)` 推目录（P2 的
  缓存副本文件名会变）。
- **ABI 版本**：`bypass_abi.h` 的 `HR_ABI_VERSION`；`HrHost`/`HrCoreApi` 都带
  `abi_version + size`，Loader 不一致拒绝加载。
- **卸载安全**：`Core_Init` 返回 0 = Core 什么都还没做，Loader 可以安全
  `FreeLibrary`；返回 1 之后 Core 可能已经起了线程，Loader **绝不能**再
  `FreeLibrary`——表不完整时只是不发布（钩子保持直通），Core 留在内存里。
- **入口补丁**：`PatchEntry5` 先挂起本进程所有其他线程；任何一个挂起/读上下文
  失败就放弃本轮重试，绝不带着"没挂上的线程"去写 5 字节 `E9`。
- **回退**：`bin\bypass_hook_single.dll` 是拆分前的单 DLL；改名覆盖
  `bypass_hook32.dll` 后**必须重启课堂客户端**（进程里已有旧 DLL），
  `inject32` 会同时识别两个文件名，避免两者同时注入打架。
- 版本自检（2.7）现在同时比较 Loader 和 Core 的 PE `TimeDateStamp`。

**扫描日志**：Loader 每次 `PatchAllModules` 在“第一次”或“本次有新模块被补”时记一行
`[Loader] IAT scan: 53 modules, 78 slots patched`。`attached: ... IAT patched (0 entries)`
里的 0 是 **Core 首次调用时的增量**（Loader 在 `Core_Init` 之前已经扫过一轮），不是漏打；
测试里能看到随后还有 `56 modules, 6 slots` 这种补扫行。

### 2.9 P2 重载语义（先定下来，P2 按这个实现）

`g_table` 只有三种状态：`NULL`（无 Core / 加载失败）、旧 Core、新 Core。

重载顺序（P2b）：

1. Loader 收到 `reload`；把新 Core 复制到 `core.<pid>.<seq>.dll`（新序号）。
2. **`g_table` 仍指向旧 Core 时**调用旧 Core 的 `before_unload()`：
   - Core 先置 `g_coreDisabled = 1`：新进来的调用在**包装 / inline hook 入口**直通
     （33 个 dispatcher 里 22 个如此，剩下 11 个只走 IAT，见 2.9 末尾）；
   - 停 Worker / caption pump / VD 线程（每个 3 秒超时，超时返回 0）；
   - 等 `g_inHookCount == 0`（3 秒超时，超时返回 0）；
   - 把状态写进 blob，返回 1。
3. `before_unload()` 返回 0：**什么都不做**，`g_table` 不动，旧 Core 继续服务。
4. 返回 1：`InterlockedExchangePointer(&g_table, NULL)` → `FreeLibrary(旧 Core)`。
   - 此时新调用全部直通（dispatcher 的 NULL 分支）；
   - inline/IAT 钩子属于 Loader，**不需要** Core 摘（也摘不了）。
5. 加载新 Core → `Core_Init`；成功则发布新 `g_table`。
6. 失败：从 `prev` 缓存副本**重新加载旧 Core** + 恢复 blob，再发布——不留在 `NULL`
   状态（否则重载失败 = 所有拦截失效）。
7. `still mapped` 检测：`FreeLibrary` 后 `GetModuleHandleA(旧缓存路径)` 必须为 `NULL`；
   非 `NULL` 说明有线程/窗口类没清干净，记 `coreDead` 并拒绝继续重载。

为了能在没有真正 `before_unload` 的情况下先验证装载/回滚，P2 分两步：
P2a = 缓存副本 + `still mapped` + 回滚框架（`before_unload` 先只 `return 1`）；
P2b = 上面第 2 步的十项真正实现。

**P2a 已实现的部分**：

- 缓存目录 `bin\core_cache\core.<pid>.<seq>.dll`：源文件 `bypass_hook_core.dll`
  从不被映射，构建输出可以随时覆盖；成功重载后删除旧缓存。
  **垃圾文件规则**：`LoadLibrary` 失败 / `Core_Init` 返回 0 → `FreeLibrary` +
  `DeleteFileA`，不留；`bad ABI` / `hooks incomplete` → 故意保留映射，留下
  `core.<pid>.<seq>.dll`，而且这两种失败没有写 `g_cur.path`，下一次 reload 的
  `DeleteFileA(prev)` 不会碰它们 → **孤儿**；`coreDead` 之后不再 `FreeLibrary`、
  不再删 `prev`，那份文件一直被映射到进程退出。`seq` 是进程内 `++g_seq`，
  所以反复失败会留下 `.1 .2 .3…`。
  自愈：Loader 的 `Manager` 启动时跑一次 `CleanOrphanCaches()`——枚举
  `core.*.dll`，按 `<pid>` 用 `OpenProcess(SYNCHRONIZE)` 判断进程是否还在，
  只在 `ERROR_INVALID_PARAMETER`（pid 不存在）时删除；被映射的文件删不掉，
  `DeleteFileA` 失败即跳过。pid 复用会漏删（留下垃圾，不是误删）。
  文件名解析是严格的 `core.<digits>.<digits>.dll`：`core.1234.dll`（缺 seq）、
  `core.abc.1.dll`、`core.0.1.dll`、`core.1.2.tmp.dll`、`other.txt` 一律**不动**
  ——「不认识的就不删」比「不认识的删掉」安全。日志只在真删了东西时打一行
  `[Loader] cache: N orphan(s) removed`（并发注入时不会每个进程都刷一行）。
- 控制管道 `\\.\pipe\bypass_core_ctl_<pid>`，一行命令 / 一行响应：
  `status`、`reload`、`reload norollback`（`norollback` 用于调试，失败时不回滚）。
  消息模式下请求超过 127 字节时 `ReadFile` 返回 `ERROR_MORE_DATA`，必须把剩余消息
  读干净再 `Disconnect`，否则客户端收不到响应（命令本身只用前缀，够解析）。
- **`FreeLibrary` 之后必须 `memset(&g_core, 0, sizeof(g_core))`**：`g_core.build_id` 和
  33 个 `hooks.*` 都指向已卸载模块；dispatcher 走 `Tbl()`（此时 NULL）不碰它，但
  `status` 命令**直接**读 `g_core.build_id`，不清就是 UAF（崩溃）。触发路径：
  `still mapped` / `reload norollback` 加载失败 / `prev` 缓存丢失 / rollback 也失败。
  清空后 `status` 输出 `core=?`。回归：`tests\status_after_failed_reload_test.ps1`
  （用 `fake_core_hooks.dll`：`Core_Init` 成功、`HooksComplete` 失败）。
  注意 `g_unloading` 仍保持 1（那几条路径都不恢复直通），所以 dispatcher 本来就不会
  再进 Core；这条只修 `status` 的直读。
- `status` 里的 `hash=0` 表示**未知**（哈希读取失败 / 刚卸载），不是真实哈希：
  UI 把它当「没有哈希」回落到 PE stamp 比较，不会误报 OLD。`g_cur.hash` 在卸载时清零。
- `core.<pid>.<seq>.dll` 的序号在有失败尝试时会跳号，这是**故意的**：`bad ABI` /
  `hooks incomplete` 会把模块留在映射状态，下一次尝试必须换新文件名，不能覆盖已映射文件。
- blob 仓库（32 项 × 256 字节）：`before_unload` **前**快照，回滚时**先恢复快照、
  再加载旧 Core**（旧 Core 的 `Core_Init` 读到的必须是卸载前的状态）。
- `still mapped` 检测的是**缓存副本路径**（`prev.cachePath`），不是源文件名；
  命中后 `coreDead = 1`、`g_table = NULL`，之后所有 `reload` 直接拒绝。
- **P2a 阶段真实 Core 的 `before_unload` 仍为 NULL**：它的 Worker/抓屏线程还没实现
  排空，`reload` 会返回 `err no-drain (P2b)`，绝不会在 P2a 里卸载真实 Core。
  P2b 实现十步之后再打开这条路径。

**P2b：`before_unload` 分两阶段**（按评审定稿）

阶段 1（只读/可逆检查，任一失败 `return 0`，**什么都没动**）：

1. `g_inHookCount == 0`，或 10ms 内能归零；仍 > 0 说明有 hook 卡住 → 拒绝；
2. Worker / caption / VD 的停止标志没有被置过（不是已在停止中）；
3. DLL 自己创建的窗口/窗口类没有未销毁的实例（`FindWindowEx` 遍历）。

阶段 2（不可逆清理）：

1. `g_coreDisabled = 1`（新调用在包装 / inline 入口直通；11 个只走 IAT 的安全类入口继续拦截，
   见 2.9 末尾）；
2. 停 Worker / caption / VD，各自 3 秒超时；
3. `RemoveWindowSubclass` / `KillTimer` / `UnregisterClass`；
4. 等 `g_inHookCount == 0`（3 秒超时）；
5. 把最终状态写进 blob（Loader 在 `before_unload` **返回 1 之后**才拍快照）；
6. `return 1`。

> 阶段 2 某一步超时返回 0 时，Core 已经置了 `g_coreDisabled`、可能停了一半线程——
> 这是不可逆操作开始后的固有风险。阶段 1 的检查目的就是把这种情况压到接近零；
> Loader 收到 0 时不卸载，旧 Core 进入**半禁用**状态：22 个入口（11 个 `HOOK_ENTER`
> 包装 + 6 个 inline + 5 个窗口状态/焦点）直通，11 个安全类入口（抓屏 5 + 关机 4 + 输入锁 2）仍会跑完整逻辑——**不是“全部直通”**
> （见 2.9 末尾）。

P2b 测试矩阵：正常重载 / Worker 卡住（`SuspendThread`）/ `g_inHookCount` 卡住
（hook 里 `Sleep(5000)`）/ 窗口未销毁 / 连续重载 3 次 / 阶段 1 失败后关闭窗口再次重载。

**P2b-1 已实现（骨架）**：

- `Core_BeforeUnload` 两阶段：阶段 1 检查 `g_inHookCount`（10ms 归零）和
  “Worker 不是已在停止中”；阶段 2 置 `g_coreDisabled`、停线程、**线程停完后
  再等一次 `g_inHookCount == 0`（50ms）**，再返回 1 —— 补掉“阶段 1 通过到
  `g_coreDisabled=1` 之间刚进来一个 hook”的竞态窗口。
- 守卫已加到 **11 个 hook**：`ShowWindow`、`SetWindowPos`、`CreateProcessW`，
  以及三个线程实际会走到的 `SetWindowLongW/A`、`SetWindowLongPtrW/A`、
  `MoveWindow`、`SwitchDesktop`、`ShowCursor`、`SetCursor`
  （`EnsureSubclassed` 走 `SetWindowLongW`，所以这组必须先有）。
- **比“只停 Worker”多停了 caption pump 和 VD 线程**：这两个线程每 250/500ms
  都会执行 Core 代码，只停 Worker 的话 `FreeLibrary` 之后它们立刻会踩到已释放
  代码。`core_host` 的实测也证明必须一起停。
- `CoInitializeEx` / `CoUninitialize` 都在 VD 线程内配对；`VD_StopWait` 只
  `WaitForSingleObject` + `CloseHandle`，不碰调用方线程的 COM。
- caption 线程不调用 VD 接口，停止顺序（Worker → caption → VD）没有反向依赖。
- 真实 Core 重载已通过：三个线程的启动标记（`attached: core_host.exe`、
  `caption pump started`、`vd: thread started`）都在 `before_unload` 之前出现，
  重载后 `still mapped` 不报错、新 Core 加载、宿主存活。

P2b-2 剩余：`RemoveWindowSubclass`、`KillTimer`、`UnregisterClass`、
`StateClose`（共享内存句柄）。`g_prevUef` 已上移。

**P2b-2 已实现的部分（ABI 3）**：

- **`g_inHookCount` 上移 Loader（双层计数）**：33 个 dispatcher
  `g_unloading ? 直通 : 计数 + 调用 Core + 减计数`；Core 的 11 个包装通过
  `HOOK_ENTER/HOOK_LEAVE` 对**同一个** Loader 计数器再加减一次。一次调用是
  `0 -> 1(Loader) -> 2(Core) -> 1 -> 0`，只有两层都返回才归零 —— 这是刻意
  更严格的 drain，不是“仅在直接调用时才有意义”。
- **`g_unloading` 在 `before_unload` 之后才置 1**：`before_unload` 内部只调
  `WaitForSingleObject`、`VD_StopWait`、`Core_RemoveSubclasses`（走
  `real_SetWindowLongPtrW`）、`StateClose`、`uef_pop`，**一个 hook 函数都不调**。
  原因见 2.9 末尾的“8 vs 25 不对称”：8 个 inline hook 会重入 Core，25 个
  IAT-only 不会（Core 模块的 IAT 被 Loader 故意跳过）。
- **Loader 侧权威 drain**：`before_unload` 返回 1、`g_unloading=1` 之后，Loader
  再等共享计数归零（最多 3s）才 `FreeLibrary`；超时返回 `err hooks-busy`
  且不卸载。
- **★ 新 Core 发布后必须把 `g_unloading` 复位成 0**（`LoadCoreFrom` 里、`g_table`
  指向新 Core 之后）。这条漏了会造成：重载"成功"但**所有** dispatcher / inline hook
  永久直通，只有 Core 的 Worker 还在工作。真实客户端上的现象：`SwitchDesktop` 不再被
  拦（桌面被切到 `NetTeachingDesktop`）、`lpDesktop` 不再被清、新窗口不再强制 window
  mode、`SetWindowsHookEx` 不再被抑制（输入可能被锁）。`tests\reload_hooks_test.ps1`
  就是这条的回归（重载前后各探一次 `WH_MOUSE_LL`：必须都被抑制、返回 `(HHOOK)1`）。
- **异常过滤器链上移 Loader**：`LoaderUefForward` 是永不更新的转发器；
  `uef_push(CoreUef)` 压栈并只在第一次 `SetUnhandledExceptionFilter(转发器)`，
  `uef_pop()` 弹栈、栈空时恢复系统原过滤器。Core 的 `CrashFilter` 只记录并返回
  `EXCEPTION_CONTINUE_SEARCH`，不再持有“前一个”指针。
- 测试补了 **“重载后 CoreUef 仍被调用”**：宿主在重载后 `RaiseException`，日志里
  必须出现该 pid 的 `!! crash`（如果链还指向已卸载的旧 Core，就不会有这行）。

**P2b-2 ③⑤ 已实现**：

- ③ 子类化：本项目的机制是 `SetWindowLongPtrW(GWLP_WNDPROC, SubclassProc)`，
  不是 `SetWindowSubclass`，所以“摘”就是恢复原过程：
  - 窗口已销毁 → `gone`；
  - 当前 WNDPROC 已被别人换掉 → `replaced`：**不碰 WNDPROC**，但仍然
    `KillTimer` + 清 `BypassOldProc`/`CAPTIMER` 属性（把我们的资源和窗口解绑），
    不阻止卸载；
  - 仍是我们 → `KillTimer` + `real_SetWindowLongPtrW(old)` + 清属性 → `removed`；
  - 恢复失败 → `failed`，`before_unload` 返回 0，Loader 不卸载。
- `SubclassProc` 的 `WM_NCDESTROY` 清理移到函数最前面，之后才看
  `g_coreDisabled`；disabled 期间其它消息直接交给原过程。
- ⑤ `StateClose`：`UnmapViewOfFile` + `CloseHandle(map/event)`，在 Worker 停止后调用。
- ④ **本项目不适用**：Core 没有 `RegisterClass`/`CreateWindowEx`，不持有自己的窗口类；
  唯一的定时器（`TIMER_CAPTION`）挂在应用的窗口上，已在 ③ 的每个窗口里
  `KillTimer`。以后如果 Core 自己创建窗口/类，再补 `DestroyWindow` +
  `UnregisterClass`。
- 测试：`core_host --window` 创建 `MultiRender*` 窗口 → 日志 `subclassed hwnd=`；
  重载时 `[Core] unsubclass: removed=1`；新 Core 重新子类化，宿主存活。

**P2b-2 加固（评审后）**：

- 33 个 dispatcher 的计数顺序改成 **先 `InterlockedIncrement`，再读
  `g_unloading` / `Tbl()`**。如果 Loader 的 drain 看到 0，说明该调用还没
  Increment，它随后一定看到 `g_unloading=1` 而直通；不存在「读到表指针、
  还没计数就被 FreeLibrary」的窗口。
- `VD_StopWait` 超时**不再关句柄/置 NULL**：句柄保留，下一次 reload 的阶段 1
  会看到「已在停止中」而拒绝，不会跳过等待去 `StateClose`。
- 阶段 1 现在同时检查 worker / caption / vd 三个停止标志；任一「停止请求已发、
  句柄还在」都拒绝卸载（半停状态不可重用）。
- 阶段 2 失败（线程/子类化超时）日志统一带 `(phase 2)`，该进程进入**半禁用**
  （部分直通、部分继续拦截，见本节末尾），应重启；阶段 1 失败则 Core 完全没动，
  可以再次 reload。
- 已知限制：`g_sub[]` 存 HWND；窗口销毁后 HWND 被复用、且新窗口恰好有同名
  属性时，`replaced` 分支的 `KillTimer`/`RemoveProp` 可能作用到新窗口。这是
  `SetWindowLongPtrW` 子类化方案的固有限制。
- **`failed > 0` 时不清 `g_sub[]`**：否则下一次 reload 看到空表会误判“都摘干净
  了”，把仍指向 `SubclassProc` 的旧 Core 卸载掉。保留表项，重试时再恢复；
  `replaced`/`gone` 的条目在成功时随整表清空。
- 机械检查（每次改 hook 后跑）：33 个 dispatcher 每个恰好 1 次
  `InterlockedIncrement`、2 次 `Decrement`、且 Increment 在 `Tbl()` 之前；
  45 个 hook/Impl 函数体内不出现
  `g_stateView|g_stateMap|g_stateEvent|g_sub|EnsureSubclassed|StateReadView`。
- **drain 用墙钟 deadline**（`GetTickCount`），不是 `N × Sleep(1)`：Windows 的
  `Sleep(1)` 实际约 15ms，3000 次会等 45 秒——3 秒超时被拖长，宿主可能先正常退出，
  reload 响应为空。Core 的 50ms drain 同理。
- **阶段 2 的顺序是「gate → 等 `g_initDepth` → 清停止标志」**：先清标志再等，会把
  Worker 在 `Core_Init` 尾巴上 re-arm 的 caption/VD 线程漏掉，3 秒等待必然失败
  （见「多实例与并发」）。`g_initDepth` 只包住 `VD_Start` 和 caption 创建两段。
- **`g_coreDead` 是 Core 契约的最终防御**：恶意 Core（`before_unload` 返回 1 但
  不停线程/不置 disabled）触发 drain 超时后，Loader 保持 `g_unloading=1`、置
  `g_coreDead=1`、**不 `FreeLibrary`**，该进程之后所有 reload 都返回 `err coreDead`，
  只能重启或重新注入。
  coreDead 之后的准确状态：dispatcher 在 `g_unloading=1` 时对所有调用直通（**Loader
  不再把任何外部调用转进 Core**）；但 Core 模块**保持映射**（drain 超时后本来就不卸载），
  所以 Core 自己的线程（Worker / caption / VD）如果还活着，会继续在自己模块里跑——
  它们用的是 `real_*`，不经过 dispatcher。结果：安全（模块没被卸，不会 UAF），
  但不干净（窗口可能还在被这个僵尸 Core 调整），终态，恢复只能重启进程。

**8 vs 25 不对称：Core 内部调用 hook 函数时谁重入**

Loader 装了两类钩子：8 个 inline hook（改函数入口）、33 个 IAT dispatcher（改导入表）。
但 `PatchOne` **跳过 Core 模块自己的 IAT**（`mod == g_coreModule`），所以：

| 函数 | Core 内部调用 | 后果 |
|---|---|---|
| 8 个 inline（`ShowWindow`、`ShowWindowAsync`、`WinExec`、`CreateProcessW/A`、`ExitWindowsEx`、`ShellExecuteA/W`） | 重入 `L_*` → 计数 → `Hook_*` | 安全，但会跑 Core 代码 |
| 25 个 IAT-only（`SetWindowPos`、`SetWindowLong*`、`MoveWindow`、`SwitchDesktop`…） | 直接走 user32/kernel32 原函数 | 不经过 dispatcher，不计数 |

重入本身在正常情况下是良性的：8 个 inline hook 的入口都在 disabled 期直通（见下），
计数只会 1→2→1→0，drain 不受影响。

**① 持锁重入审计（P3 补充）**：Core 里 5 把锁全是 `CRITICAL_SECTION`
（`g_subCs` / `g_envCs` / `g_geoSs` / `g_vdCs` / `g_scCs`），Windows 的 CS 对同一线程可
重入，所以「重入另一个也拿同一把锁的 hook」不会死锁。逐个体检 8 个 inline hook：
没有一个是**持锁时调用 inline hook 目标**；`Hook_CreateProcessA/W` 确实把 `g_envCs`
跨过 `real_CreateProcessX`（环境变量必须只在那次调用期间生效），这是有界争用（另一个
CreateProcess 会等这次创建完成），不是死锁。唯一同线程阻塞是 `VD_PinNow`（`ShowWindow`
路径）：`VD_WaitReady` 最多自旋 500ms + `WaitForSingleObject(g_vdAck, 250)`，都远小于
drain 的 3 秒预算。

**半禁用的准确语义（阶段 2 失败 / drain 超时）**

33 个 dispatcher 里的分布（P3 起）：

| 类别 | 数量 | disabled 期行为 |
|---|---|---|
| `HOOK_ENTER` 包装 | 11 | 入口查 `g_coreDisabled` → **直通** |
| inline hook 里原先没包装的（`ShowWindowAsync`、`CreateProcessA`、`WinExec`、`ExitWindowsEx`、`ShellExecuteA/W`） | 6 | 显式查 `CORE_DISABLED()` → **直通** |
| 窗口状态/焦点（`SetWindowPlacement`、`CreateWindowExW`、`SetForegroundWindow`、`BringWindowToTop`、`SetActiveWindow`） | 5 | **直通**（`CreateWindowExW` 例外：只保留「隐藏 ShutDownDlg 倒计时窗」这条关机安全逻辑，桌面钉住直通） |
| 抓屏守卫（`BitBlt`、`CreateDCW`、`CreateDCA`、`GetDC`、`GetWindowDC`） | 5 | 跑完整逻辑 |
| 关机入口（`InitiateSystemShutdownA/W/ExA/ExW`） | 4 | 跑完整逻辑（守卫开时仍拦） |
| 输入锁（`SetWindowsHookExA/W` 拦 `WH_MOUSE_LL` / `WH_KEYBOARD_LL`） | 2 | 跑完整逻辑 |

也就是 disabled 期只剩**安全类 hook** 在跑：抓屏冻结、关机入口拦截、低层输入钩子抑制；
其余（窗口显示/焦点/摆放、进程创建、外壳、`ExitWindowsEx`）全部直通。
`SetWindowsHookEx` 故意留在安全侧：它挡的是客户端锁键盘/鼠标的通道，
disabled 期放通等于可能被锁输入，比“点窗口没反应”严重。

「重载失败」≠「hook 停了」：窗口操作类都直通了，但**抓屏冻结仍在**、
`InitiateSystemShutdown*` 仍被拦、**输入锁仍被抑制**。UI、日志、README 都按这个描述，
文案里「still run」的三类必须穷举（不能只说“screen guard”）。`g_coreDead` 是该状态下的终态。

**直通不变式的测试覆盖（known gap）**

8 个 inline hook 的「disabled 期直通」里，只有 `WinExec` 有自动测试
（`tests\reentry_test.ps1`：守卫开着时 `ret=33` + `[Guard] blocked WinExec`，
半禁用时 `ret=2`（`ERROR_FILE_NOT_FOUND`）+ 无新拦截行）。其余 5 个
（`ShowWindowAsync`/`CreateProcessA`/`ExitWindowsEx`/`ShellExecuteA/W`）和 7 个
窗口状态/焦点 hook **只有代码审查覆盖**：`ExitWindowsEx`/`ShellExecute` 构造零副作用
探针很难，`ShowWindowAsync` 需要目标进程泵消息。改动这 12 个入口时必须人工确认
`CORE_DISABLED()` 仍在函数最前面。

**UI 入口（主程序 / 托盘）**

主界面「重载 Core」按钮和托盘「重载 Core（热更新）」都调 `OnReloadCore`：

- 目标 PID 来源：按 `RELOAD_TARGETS` 里的课堂进程名 `Process32First/Next` 匹配，
  再连 `\\.\pipe\bypass_core_ctl_<pid>`；连不上 = 没注入/旧 DLL，记日志跳过。
  不做全进程扫描（慢、且容易触发杀软）。
- 线程：`ReloadCoreThread` 后台跑，`g_reloadBusy` 防重入；15 秒 `PeekNamedPipe`
  轮询（100ms 一次）。
- 时间预算对齐：Loader 最坏 ~12 秒（3s Worker + 3s caption + 3s VD + 3s drain），
  所以 3 秒时 `IDT_RELOAD_STATUS` 只把状态栏切成 `reloading Core (up to 15s)`，
  **不判失败**；只有 `err*` 响应才算失败。
- 结果分类：`ok` / `rolledback`（新 Core 失败、旧 Core 已恢复）/
  `refused`（阶段 1 拒绝，什么都没动）/ `dead`（含 `core disabled`、`coreDead`，
  弹“部分直通”警告并把状态栏改成 `Core disabled, restart the process`）。
  状态串控制在 ~45 字符内，状态栏用 `DT_END_ELLIPSIS`，再长就会被截断。

**版本自检（P3）：内容哈希，PE stamp 只作兜底**

「检查当前状态」判断「这个进程里的 DLL 是不是磁盘上那份」时，**优先用内容哈希**：

- Loader 在 `CopyFileA(src, cache)` 之后对 **cache 副本**算 FNV-1a 32，也就是它
  实际 `LoadLibraryEx` 的那份字节；日志 `[Loader] core loaded: ... hash=xxxxxxxx`，
  控制管道 `status` 回 `hash=xxxxxxxx`。
- UI 算 `bin\bypass_hook_core.dll` 的 FNV，和 `status` 里的比：相等 = current。
- **为什么不用 PE `TimeDateStamp`**：它是**链接时间**，不是身份。同一秒内重编两次、
  `SOURCE_DATE_EPOCH` 可重复构建、手工改过的 DLL，stamp 都不变——会误报 current。
  而热重载之后进程里跑的是 `core.<pid>.<seq>.dll`，问题本来就是「哪份字节」。

三层判据：

| 情况 | 判据 | 建议 |
|---|---|---|
| `hash` 相同 | 内容一致 | 无事 |
| `hash` 不同，磁盘稳定 | Core 旧了 | **点「重载 Core」**（Loader 是新的，不用重启） |
| `hash` 不同，磁盘两次读还不一样 | 构建正在写这个文件 | `disk is being rewritten - check again`，不给建议 |
| `status` 超时且 `g_reloadBusy` | Loader 正在 reload（单连接管道） | `reloading Core (Loader busy)`，不落回 stamp |
| 没有控制管道 | 旧单 DLL / P2 之前 | 退回 PE stamp 两个层都比，结论一律「重启课堂客户端」 |

> 构建脚本**不是原子写**（`gcc -o "$@"` 直接 truncate 目标文件），所以第二次哈希
> 确认是必须的，否则构建中途点检查会看到半成品、建议重载半成品。

> 顺带修掉一个真 bug：`PeStampLoaded` 原来只用 `TH32CS_SNAPMODULE`。主程序是 64 位、
> 课堂进程是 32 位，从 64 位进程枚举 32 位模块必须带 `TH32CS_SNAPMODULE32`，否则
> `Module32First` 直接失败 → 每个进程都显示 `not loaded`，旧单 DLL 的兜底形同虚设。

> Loader 在拷贝后还会比一次 `hash(cache) vs hash(src)`，不同就记
> `core hash mismatch ... (DLL rewritten during the copy?)`——这是「替换 DLL 与重载
> 并发」的唯一进程内证据。

**多实例与并发（C 组验证）**

控制管道是**单连接**的（`nMaxInstances=1` + `PIPE_WAIT`），所以第二个客户端会拿到
`ERROR_PIPE_BUSY`——这**不是**「没注入」。`CtlRequest` 用 `WaitNamedPipeA` 等待并复用
同一个超时预算；等不到才返回 `busy`/`-1`。UI 早先把它当 0 会误报 “not injected”。

**Core 的「初始化 vs 卸载」竞态**（`multi_test` 抓到的真实缺陷）：`Core_Init` 里
Worker 有 `Sleep(200)`，之后才 `VD_Start()` 和创建 caption 线程。若 reload 恰好落在
这一段，旧顺序（先清 `g_captionRunning`）会把随后 re-arm 的线程漏掉，线程永不退出 →
phase 2 3 秒超时（实测日志 `caption pump did not stop in 3s (wait=258)`）→ reload 失败。

修复是 `g_initDepth` 握手，且**顺序必须是「先 gate，再等，最后清」**：

```
Worker 建线程:   depth++  ->  已 gate 就 depth-- 放弃  ->  建线程  ->  depth--
phase 2:         g_coreDisabled=1  ->  等 depth==0（1s）  ->  清 worker/caption 标志
```

顺序反过来（先清标志再等 depth）仍会漏掉「已经 re-arm 的线程」。`g_initDepth` 只覆盖
`VD_Start` 和 caption 创建这两段（Worker 自己是 `Core_Init` 同步创建的，Loader 不会在
`Core_Init` 返回前调 `before_unload`）。

**每个进程独立的状态**：`g_inHookCount`、UEF 栈、blob 仓库、缓存目录
（`core.<pid>.<seq>.dll`）、控制管道名（带 pid）。多开不需要共享任何东西，
一次 UI 点击 4 个进程全部 `ok seq=2`，各自 hash 与磁盘一致、旧 `.1.dll` 被删；
一个进程崩溃时 `!! crash` 只出现在该 pid，其余进程仍然 `dead=0`。
同一个 pid 收到两个并发 `reload` 会被管道串行化，两个都成功（seq=3、seq=4），
不会交错。

> 未覆盖：真实多桌面会话、真实课堂客户端的 4 个进程同时广播（需要另一台/另一个
> 桌面环境）。

---

## 3. 日志与排错

日志文件：
- `bypass_hook.log` —— **唯一的日志文件**。注入的 DLL（每个被注入进程一份输出，行首格式如下：
  `[HH:MM:SS.mmm] [pid N] 内容`，时间戳用于判断"最大化→还原"隔了多久、定时器有没有在跑）和
  主程序的启动/提权过程（`launcher ...`、`lpe: ...`、`[elevate] ...`、
  `elevated instance pid=... is up`）都写在这里。
  **清空只发生在 `bypass.exe` 启动时**（`TruncateHookLog` → `CREATE_ALWAYS`，提权子实例
  不截断，见 4.4）；注入的 DLL 侧 `HookLog` 用 `FILE_APPEND_DATA + OPEN_ALWAYS`，
  **只追加**，跨进程、跨重启持续增长。手动删掉文件后，下一次 `HookLog` 会重建（每 64 行
  检查一次句柄），所以"删日志"立即生效，而不是等下次启动。
- 主程序界面日志 —— 按钮操作、扫描结果、被降级的窗口、注入器的 stdout
  （**不再生成 `bypass_inject.log`**：注入器输出走匿名管道直接进界面/日志）

常见关键字：

> 改写类日志（`ShowWindow(5) -> SW_SHOWNOACTIVATE`、`forced SWP_NOACTIVATE`、
> `suppressed ...`、`user left/back`）做了限流：同一个（窗口, 动作）15 秒内只打一条，
> 状态切换 3 秒内只打一条。渲染器每秒会调十几次，不这样会把整份日志淹掉。

| 关键字 | 含义 |
|---|---|
| `attached: <exe> - IAT patched (N entries)` | 注入成功，共 patch 了 N 个导入项 |
| `inline hook (hotpatch): ... -> ... (real ...)` | 用函数入口前的 5 字节填充位挂的内联钩子（入口只写一条 2 字节 `jmp short`，不会执行到半条指令） |
| `inline hook installed: ... (trampoline ...)` | 序言不是 hotpatch 形状（`B8`/`68` 等）时退回"搬 5 字节到 trampoline"的写法 |
| `broadcast_proc=1/0` | 本进程是否被判定为"全屏广播渲染器" |
| `fix hwnd=... 1918x964` | 首次降级某个窗口 |
| `fix(foreign, async) hwnd=...` | 降级**别的进程**的窗口：只用 `SWP_ASYNCWINDOWPOS` 改几何，不碰样式（`SetWindowLong` 会同步等对端 UI 线程） |
| `-> now 868x467 (wanted 868x467)` | 降级后回读，尺寸对得上 |
| `!! readback mismatch` | 改完回读不一致 → 另有东西在改这个窗口 |
| `keep user geometry ... (app wanted ...)` | 应用想改尺寸/位置，被冻结 |
| `ignore app window-state change ... (cmd=9)` | 应用想还原/最大化，被忽略（9 = SW_RESTORE） |
| `user restore ... (done by us)` | 用户点还原，由我们摆回用户矩形 |
| `user maximize ... (done by us)` | 用户点最大化，由我们执行 |
| `user-resized hwnd=...` | 用户拖完，记录为新的"用户矩形"（拖到接近满屏也算） |
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

验证"屏幕监控守卫"（`make.bat screentest` + 脚本）：

```bat
make.bat all screentest
powershell -NoProfile -ExecutionPolicy Bypass -File tests\screenguard_test.ps1
```

脚本用 `tests\screenguard_test.c` 复刻 `StudentLogic.dll` 的抓屏顺序
（`CreateDCW("DISPLAY") → BitBlt → GetDIBits`）：先把窗口填成颜色 A 抓一次，再换成颜色 B
抓一次。`enabled=0` 时两次指纹不同（实时），`enabled=1` 时两次相同（定格帧）：

```
case 1/2: guard OFF (expect frozen=0)
  off: frozen=0
case 2/2: guard ON  (expect frozen=1)
  on: frozen=1
VERDICT PASS
```

验证"屏蔽控制端重启/关机"（`make.bat shuttest` + 脚本，**建议在虚拟机里跑**）：

```bat
make.bat all shuttest
powershell -NoProfile -ExecutionPolicy Bypass -File tests\shutguard_test.ps1
```

脚本对 `enabled=0/1` 各注入一次 DLL，只操作 `%TEMP%\shutguard_probe\shutdown.exe`
（`cmd.exe` 的重名副本，执行它什么也不做）：

```
case 1/2: guard off -> fake shutdown.exe 正常启动；命令行透传
case 2/2: guard on  -> 所有 shutdown 写法都被吞掉，不创建进程
                      日志里有 [Loader] inline hooks: all installed + [Loader] core loaded
VERDICT PASS
```

> 脚本运行前会扫描 `bin\shutguard_test.exe`：只要里面出现 `ExitWindowsEx` 导入、
> `InitiateSystemShutdown` 或裸的 `shutdown -s|/s|-r|/r|-t` 字面量就直接拒绝运行。
> `ExitWindowsEx(0x80000000, 0)` **不是**"无害探针"——Windows 会当成关机请求，
> 开发时已经因此关过机器，绝对不要再加回去。

验证 Loader 的失败路径（`make.bat fakecores` + `tests\fake_core_test.ps1`）：

```bat
make.bat all fakecores
powershell -NoProfile -ExecutionPolicy Bypass -File tests\fake_core_test.ps1
```

脚本把 `bin\bypass_hook_core.dll` 临时换成三个故意损坏的假 Core，各跑一次
`wmode_test --inject`：

| 假 Core | 期望日志 | 期望行为 |
|---|---|---|
| `Core_Init` 返回 0 | `Core_Init failed (returned 0) - FreeLibrary safe` | 可安全卸载，表 NULL，直通 |
| 返回 1，`abi_version=999` | `Core_Init returned bad ABI ... leaving it mapped` | **不卸载**，表 NULL，直通 |
| 返回 1，ABI 对，hook 表缺字段 | `Core hook table incomplete ... leaving it mapped` | **不卸载**，表 NULL，直通 |

后两种假 Core 会起一个心跳线程写 `%TEMP%\bypass_fake_core_beat.txt`；脚本检查它存在，
证明 `Core_Init` 返回 1 后 Loader 没有 `FreeLibrary`（否则线程会执行已释放代码、进程崩）。
最后恢复真 Core，`wmode` 必须回到 `FIXED`。

验证 P2a/P2b 的缓存副本 / 重载 / 回滚 / 子类化 / UEF（`make.bat fakecores corehost` + 脚本）：

```bat
make.bat all fakecores corehost
powershell -NoProfile -ExecutionPolicy Bypass -File tests\reload_test.ps1
```

脚本启动一个自注入 Loader 的长驻 `core_host.exe`，通过
`\\.\pipe\bypass_core_ctl_<pid>` 驱动：

| 场景 | 期望 |
|---|---|
| 首次加载 | `bin\core_cache\core.<pid>.1.dll` 存在，`status` 返回 `seq=1` |
| 重载成功 | `ok seq=2`，新缓存存在，旧缓存被删 |
| 窗口子类化 | 日志 `subclassed hwnd=` → 重载 `[Core] unsubclass: removed=1` → 新 Core 重新 `subclassed` |
| 重载期间后台 hammer `ShowWindow` | 宿主存活，新 Core 正常（`--hammer-show`） |
| 初始加载失败后 reload | `err load-failed` → 换回真 Core 后 `ok seq=N` |
| 新 Core `Core_Init` 返回 0 | `rolledback`，日志有 `reload failed: Core_Init returned 0` 和 `rollback ok` |
| 新 Core 返回 1 但 ABI 不对 | `rolledback`，日志有 `bad ABI ... leaving it mapped`（不卸载它） |
| 装一个“泄漏” Core（自己多 `LoadLibrary` 一次）再重载 | `err coreDead`，日志有 `still mapped after FreeLibrary`，`status` 里 `dead=1`，进程不崩 |
| 重载后 `RaiseException` | 新 Core 的 `CrashFilter` 记录该 pid 的 `!! crash`（UEF 链没指向旧 Core） |

验证阶段 1/2 的失败路径（`make.bat testcores corehost` + 脚本）：

```bat
make.bat testcores corehost
powershell -NoProfile -ExecutionPolicy Bypass -File tests\stuck_test.ps1
```

三个测试构建（`-DBYPASS_STUCK_HOOK` / `_WORKER` / `_SUBCLASS_FAIL`，只影响测试 DLL）：

| 场景 | 断言 |
|---|---|
| hook 卡死（第一次 `ShowWindow` Sleep 5s） | reload → `err refused-by-core`，日志 `hooks still executing`；5s 后再次 reload → `ok seq=` |
| Worker 卡死（收到停止请求后 Sleep 60s） | reload → 0（阶段 2 超时），日志 `(phase 2): worker did not stop`；再 reload 被阶段 1 拒绝；进程存活 |
| 子类恢复失败 | reload → 0（阶段 2），日志 `(phase 2): a window still points`；再 reload 仍是阶段 2 失败；进程存活 |

验证「恶意 Core」（`make.bat testcores` 也构建 `core_evil.dll`，脚本 `tests\evil_core_test.ps1`）：

| 场景 | 断言 |
|---|---|
| `before_unload` 返回 1 但不停线程（线程卡在 hook 里） | reload → `err hooks-busy (core disabled)`；日志 `reload aborted ... core disabled`；`status` 里 `dead=1`；再 reload → `err coreDead`；进程存活 |

> `g_coreDead` 是终态：该进程不能恢复，测试必须用独立宿主、且不包含“恢复后 reload”的断言。

**一键回归**（`make.bat` 目标 + 全部 PowerShell 套件）：

```bat
:: 构建 + 跑全部套件
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_all.ps1
:: 用现有 bin\ 直接跑（不重新编译）
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_all.ps1 -NoBuild
:: 只跑名字匹配的套件
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_all.ps1 -NoBuild -Filter *reload*
```

`run_all.ps1` 先构建 `all / fakecores / testcores / corehost / uitest / wmodetest /
screentest / shuttest`，然后依次跑 13 个套件：

| 套件 | 覆盖 |
|---|---|
| fake cores | Loader 加载失败路径（return0 / bad ABI / hooks 不全 / 泄漏） |
| reload | 缓存副本、回滚、`still mapped`、UEF、窗口重载、hammer |
| stuck | hook / worker / subclass 三组卸载失败 |
| 恶意 Core | drain 超时 → `coreDead` 终态（独立宿主） |
| wmode | 置顶剥离、z 序冻结、用户 resize |
| screenguard | 冻结首帧 |
| foreign_fix | 跨进程兜底不卡死 enforcer |
| shutguard | 关机/重启拦截 |
| reentry | 阶段 2 失败后 6 个 inline hook 直通（半禁用期的 hook 语义） |
| reload_hooks | 重载成功后 hook 立即恢复（g_unloading 复位；WH_MOUSE_LL 探针） |
| status_uaf | 卸载成功 + 新 Core 加载失败后 `status` 返回 `core=?`（不读悬空 build_id） |
| version | P3 自检：current / OLD+「重载」/ 磁盘重写不给建议 |
| multi | 4 进程一次点击、管道并发、慢 reload 期间的状态检查、崩溃隔离 |

任一失败退出码非 0。恶意 Core 那组是终态，脚本把它排在 reload 系最后，且后面不再做
“恢复后重载”断言；multi 那组会临时换 Core 文件，结束时恢复。

验证"跨进程兜底不会卡死 enforcer"（`make.bat hangwintest` + 脚本）：

```bat
make.bat all hangwintest
powershell -NoProfile -ExecutionPolicy Bypass -File tests\foreign_fix_test.ps1
```

脚本把 `bin\hangwin_test.exe` 复制成 `%TEMP%\bypass-foreign-test\ScreenRender.exe`
再启动：它建一个 `MultiRender*` + 95% 屏的窗口，然后**挂起自己的主线程**
（永不泵消息）。随后在该目录跑 `wmode_test --inject`，断言：

- `wmode_test` 在 60 秒内跑完且自身通过（旧版同步兜底会把 enforcer 卡住）；
- 日志里出现 `fix(foreign, async) hwnd=... 2432x1368 -> ...`。

> 受害者必须是 **64 位**：我们的注入器是 32 位的，一旦把 DLL 注进受害者，
> `PidHasDll()` 就会让 enforcer 走"对端自己有钩子"的分支，跨进程兜底根本不会执行。
> 没有 64 位工具链时脚本会打印 `SKIP`。

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