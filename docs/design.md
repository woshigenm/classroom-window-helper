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

### 1.7 小窗化后自动解锁键盘

"全屏广播已经被我们降成小窗"是明确的接管信号：主程序在两条路径上补发一次
`IOCTL 0xB2008`（`UnlockKeyboardQuiet()`，幂等，需要管理员 + 厂商过滤驱动）：

- `DemoteWindowTiered()` 每次降级**成功**后立即发一次（事件驱动路径，最及时）；
- 小窗线程 `SmallWinThreadProc` 每轮枚举后若看到"已接管的小窗"（`ctx.small > 0`）再补发一次，
  并在**首次**发现时写一行 `[Keyboard] broadcast is now a small window -> auto unlock ...`。

`AutoUnlockAfterSmallWin()` 用 `GetTickCount()` 做 3 秒节流，返回 `1/0/-1`（已发成功 / 已发失败 /
3 秒内已发过），避免 `DefineDosDevice` 与日志被反复触发。

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
[DesktopGuard] input='MCLOCK Desktop' -> switched to Default, keyboard unlock sent (0xB2008)
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

### 2.1c 应急点击（主程序内）

`MultiClient.exe` 的 `WH_MOUSE_LL` 只放行 `dwExtraInfo == 0x12452` 的事件。
`Ctrl+Alt+E` 直接借用这个通行证发一次左键点击，鼠标被冻结时也能点一下。

### 2.2 配置接管（主程序内）

开启「实时解锁监控」后，主程序每 10 秒检查 `skin\core.conf`：

| 键 | 期望值 |
|---|---|
| `Limit` | `0` |
| `LockSeat` | `0` |
| `LockedAfterNetBroken` | `0` |

被教师端改回非 0 就自动重写，并记一行：

```
[Config] limiter keys were set again by the classroom tool -> reset to 0
```

> 只在「键存在且非 0」时动手；键不存在绝不当作"需要改写"，否则会无限重写文件。


---

## 3. 日志与排错

日志文件：

- `bypass_hook.log` —— DLL 写的（每个被注入进程一份输出，行首格式如下）：
  `[HH:MM:SS.mmm] [pid N] 内容`（时间戳用于判断"最大化→还原"隔了多久、定时器有没有在跑）
- 主程序界面日志 —— 按钮操作、扫描结果、被降级的窗口、注入器的 stdout
  （**不再生成 `bypass_inject.log`**：注入器输出走匿名管道直接进界面/日志）

常见关键字：

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
| `STUCK ... owner hook missing?` | 某个窗口所在的进程还没被注入（跨进程兜底用尽） |
| `caption timer armed` / `caption timer first tick` | 标题栏定时器装上 / 第一次触发。只有 armed 没有 first tick → 渲染器不取 `WM_TIMER`，由 250ms 心跳兜底 |
| `caption tick hwnd=... pt=... band=...` | 每秒一行的判定现场：光标在不在标题栏带里、`idle` 多久了 |
| `caption hide due` → `caption auto-hide` | 1.2 秒到了，真正摘掉 `WS_CAPTION` |
| `caption tick skip (...)` | 被开关/接管状态挡住（括号里直接给出 disabled/autohide/managed/iconic/visible） |

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
cd /d <bypass_tool>
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

---


---

## 6. 免责声明

本工具仅用于**自己拥有/被授权管理**的机房与教学环境，用于改善学生端可操作性。
请遵守所在学校/单位的使用规定与当地法律。