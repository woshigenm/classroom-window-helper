# 噢易多媒体网络教室 V10.8 —— 学生端「小窗化广播」工具

把教师端的**全屏广播**变成学生端**可拖动 / 可缩放 / 可最大化**的普通窗口，
并默认把广播窗钉在一号虚拟桌面（切到别的桌面后再广播，不会被拽回去）。

原理见 [`docs/design.md`](docs/design.md)，上手 / 快捷键 / 故障排查见 [`docs/how-to-use.md`](docs/how-to-use.md)。

## 1. 交付文件（都在 `bin\`，必须同一目录）

| 文件 | 作用 |
|---|---|
| `bin\bypass.exe` | 主程序：界面、托盘、看门狗、注入调度 |
| `bin\bypass_hook32.dll` | **Loader**：注入课堂进程，装钩子、打 IAT、持有日志/路径；逻辑改动时不用换它 |
| `bin\bypass_hook_core.dll` | **Core**：所有窗口/抓屏/关机守卫的实现；改逻辑只换这个 |
| `bin\bypass_hook_single.dll` | 回退用的旧版单 DLL（P1 之前那版），把名字改回 `bypass_hook32.dll` 即可回退 |
| `bin\inject32.exe` | 注入器（主程序自动拉起，也可手动） |
| `bin\lpe\` | 启动时的自动提权载荷（可选；没有它就走 UAC） |

> ⚠ `bin\` 必须在本机磁盘。VMware 共享目录 / 网络盘不支持 PE image section 映射，
> 目标进程里 `LoadLibraryW` 返回 0，注入必失败。

## 2. 快速开始

1. 把仓库复制到本机（如 `C:\classroom-window-helper\`）；没有 `bin\` 先按第 3 节编译。
2. 把 `bin\` 加进 Defender 排除项（注入的 DLL 容易被判 HackTool 直接删）。
3. 双击 `bin\bypass.exe`：默认先**自动提权**（见 5.3），失败弹 UAC，再失败才提示
   「右键 → 以管理员身份运行」。托盘出现图标即启动。
4. 教师端开始广播，学生端自动把广播窗降为小窗。
5. 退出：托盘菜单「退出」或 `Ctrl+Alt+Q`。

最小化 / 关闭 = 收进托盘（窗口可见时任务栏上也有按钮）。找回窗口：托盘图标、
`Ctrl+Alt+H`，或再双击一次 `bin\bypass.exe`（不会重复启动，也不会重跑提权）。

## 3. 编译

`make.bat` 用 `setlocal` 临时把工具链加进 PATH，不改系统环境：

```bat
make.bat              :: 主程序 + DLL + 注入器 + lpe 载荷
make.bat bypass       :: 只编主程序
make.bat hook32       :: 只编 DLL + 注入器
make.bat lpe          :: 只编 / 铺开 bin\lpe\
make.bat test         :: bin\hooktest32.exe
make.bat wmodetest    :: bin\wmode_test.exe
make.bat vdtest       :: bin\vd_pin_test.exe
make.bat hangwintest  :: bin\hangwin_test.exe（跨进程兜底测试用的"不泵消息"受害者）
make.bat screentest   :: bin\screenguard_test.exe（屏幕监控守卫自测载体）
make.bat fakecores | testcores | corehost   :: 失败注入用的假/坏 Core、长驻宿主
make.bat clean | rebuild | check
```

跑全部回归（构建 + 12 个套件，任一失败退出码非 0）：

```bat
powershell -NoProfile -ExecutionPolicy Bypass -File tests\run_all.ps1
```

`-NoBuild` 用现有 `bin\` 直接跑，`-Filter *reload*` 只跑名字匹配的套件。
其中 `version` 覆盖「检查当前状态」的内容哈希判据，`multi` 覆盖 4 进程同时重载、
控制管道并发、慢 reload 期间的状态检查与崩溃隔离。

- 工具链：32 位 `i686-w64-mingw32-gcc`（课堂软件全是 32 位，主程序也用 32 位编），
  `make.bat` / `Makefile` 自动探测 RedPanda-Cpp 与 `C:\mingw32`；直接
  `mingw32-make` 也能编（Makefile 自己把工具链目录导出到 PATH，gcc 才找得到
  `cc1 / as / ld / collect2`）。
- DLL 必须 `-static -static-libgcc`（脚本已带），否则目标进程 `LoadLibraryW` 返回 0。
- **编码**：源码和文档统一 UTF-8（无 BOM），`.bat` 保持纯 ASCII（cmd 按 OEM 代码页解析）。
  用 GBK/ANSI 另存会把中文注释和日志文案变成问号，编译器不报错；仓库根的
  `.editorconfig` 已经把编码固定住，编辑器会自己遵守。
- `bin\` 在 `.gitignore` 里；要随仓库发布就用 GitHub Release 附件。

## 4. 目录结构

```
classroom-window-helper\
├─ .editorconfig                编码 / 换行约定（UTF-8，.bat 纯 ASCII）
├─ make.bat  Makefile          构建入口（工具链自动探测）
├─ src\                        交付组件源码
│   ├─ bypass.c                主程序：界面 / 托盘 / 注入调度
│   ├─ bypass_hook.c           注入 DLL（含 bypass_vd.inc：虚拟桌面固定）
│   ├─ inject32.c              注入器
│   ├─ elevate.c / elevate.h   启动提权：lpe / uac / manual / off
│   ├─ lpe_broker.c            自动提权用 shell_broker.exe
│   └─ bypass_state.h          主程序 / DLL 共享状态
├─ lpe\                        自动提权载荷（trigger.ps1 + payload\ 里的 PoC 原样二进制）
├─ tests\                      自测源码（hooktest32 / wmode_test / vd_pin_test / hangwin_test）
├─ build\                      编译 / 签名 / 打包脚本（说明见 build\README.md）
├─ docs\                       how-to-use.md（手册）、design.md（原理）
└─ bin\                        所有编译产物 + 运行期配置 / 日志（gitignore）
```

## 5. 运行期配置与安全阀

`bin\bypass_config.ini`（自动生成，改完 1 秒内生效）：

| 键 | 作用 |
|---|---|
| `window_mode` | `1`（默认）窗口模式；`0` 关闭 |
| `title_autohide` | `1`（默认）最大化后自动隐藏标题栏 |
| `minimize_mode` | `taskbar`（默认，真最小化）/ `tray`（收起不占任务栏） |
| `small` | 空 = 默认；`0.35` = 屏宽比例；`640x360` = 固定外框 |

安全阀：`bin\bypass_hook.off` 存在 = 暂停所有钩子（退出工具不写，启动时自动清理）。

### 5.1 虚拟桌面固定（`bin\bypass_vd.ini`）

`enabled=1`（默认）把广播窗钉在 `desktop=0`（一号桌面）；你在别的桌面时重新广播，
窗口只显示不激活，不会被拽回去。`enabled=0` = 老行为（跟着当前桌面走）。
判定逻辑、改写规则和日志行见 `docs\design.md` 2.3。

### 5.2 窗口模式广播的小窗

老师用**窗口模式**发广播时，小窗出生就带置顶；工具会摘掉置顶并冻结 Z 序，
你自己点它仍然能提到前面（详见 `docs\design.md` 2.4）。无需配置。

### 5.3 启动方式与自动提权（`bin\bypass_elevate.ini`）

`mode=auto`（默认，先 LPE → 再 UAC → 再手动指引）| `lpe` | `uac` | `manual` | `off`；
命令行 `--elevate=xxx` 单次覆盖。没有 `bin\lpe\` 时等价于 `uac`（旧行为）。

- LPE 走 `bin\lpe\`（WalletService 链路，**2026 年 7 月已修复**的漏洞）：补丁机上必然失败，
  会自动退回 UAC，属设计内行为。链路细节 / 编码约定 / broker 令牌见 [`lpe/README.md`](lpe/README.md)。
- 提权与 DLL 日志合并成一个文件 `bin\bypass_hook.log`（启动器每次启动重写，broker 拉起的
  实例不重写，链路日志连续）。`--elevate-selftest` 只把 Base64 载荷写到 `%TEMP%`，不启动任何东西。

### 5.4 屏幕监控守卫（`bin\bypass_screen.ini`）

主界面第 3 行按钮「屏蔽屏幕监控」。开了之后，老师端的"屏幕监控/录屏"只能看到**一张定格画面**，
我们实时的桌面不再上传。

```ini
; bin\bypass_screen.ini（点按钮自动生成，DLL 每 0.5 秒重读，改完不用重启）
enabled=1
mode=freeze     ; freeze=第一帧当定格帧（默认） / blank=固定纯色
```

- 原理：学生端抓屏只有一条 GDI 路径（`StudentLogic.dll` 的 `CreateDCW("DISPLAY") → BitBlt →
  GetDIBits`，REA/Ghidra 实证），注入的 DLL 钩住 `BitBlt`：**第一次**抓屏存一帧，之后每次抓屏
  都从缓存画出来，屏幕一个像素都不再读。
- 生效前提：`bypass_hook32.dll` 已经在 `Student.exe` / `ScreenRecord.exe` 里（点按钮会自动
  调注入器；你也可以看日志里的 `[Screen] guard ON/OFF`）。
- 只覆盖 GDI 抓屏；换成 DXGI / `PrintWindow` 抓屏就拦不到。定格后画面完全不动，老师端容易
  看出是静态图——这是"画面固定"的固有代价（详见 `docs\design.md` 2.6）。

### 5.5 屏蔽控制端重启/关机（`bin\bypass_shut.ini`）

主界面勾选框「屏蔽控制端重启/关机」。勾上之后，老师端菜单里的**远程重启 (0x27)** 和
**远程关机 (0x28)** 都会被吞掉：本机不关机、不重启，只在 `bin\bypass_hook.log` 里留一行：

```
[Guard] blocked ExitWindowsEx flags=0x6 (restart)      ← 0x27 重启
[Guard] blocked ExitWindowsEx flags=0x1 (shutdown)     ← 0x28 关机（如果走这个 API）
[Guard] blocked CreateProcessW (shutdown -s -t 1)      ← 0x28 关机（走 shutdown.exe）
```

`flags` 一眼就能分清老师点的是重启还是关机。前 3 次逐条记，之后每 25 次只记一行
`blocked N remote power commands (last: ...)`，不会刷爆日志。

面板收进托盘时点不到勾选框：**右键托盘图标**，菜单里的「屏蔽控制端重启/关机」是同一个开关
（带勾选状态）。

```ini
; bin\bypass_shut.ini（点勾选框自动生成，DLL 每 0.5 秒重读，改完不用重启）
enabled=1
```

- 拦截点：`ExitWindowsEx`（0x27 的直接调用）、`CreateProcessA/W` / `WinExec` /
  `ShellExecuteA/W`（0x28 的 `shutdown -s -t 1`）、`InitiateSystemShutdown*`（未使用，兜底）。
- 0x28 的**倒计时窗**（`ShutDownDlg`）也会被隐藏：关机已经拦了，再弹一个
  “系统将在 N 秒后关闭”只会让人以为工具坏了。
- 被拦的调用会返回"成功"（`TRUE` / `33`），控制端不会当成失败反复重发。
- 只影响课堂进程（`Student.exe`、`MultiClient.exe` 等被注入的进程）；**你自己点开始菜单关机
  不受影响**。
- `systemoper.dll` 是运行时才加载的，所以用的是函数入口 inline hook，不是只改 IAT
  （见 `docs\design.md` 2.7）。
- 版本自检：主界面「检查当前状态」会列出每个课堂进程里 `bypass_hook32.dll` 的构建时间，
  和磁盘上的 DLL 比对；不一致会提示 **restart the classroom client**——重启 `bypass.exe`
  不会替换已经注入的旧 DLL。

### 5.6 Loader / Core 分层

注入的 DLL 拆成两层：

| 层 | 文件 | 负责 |
|---|---|---|
| Loader | `bin\bypass_hook32.dll` | 装所有 inline/IAT 钩子、持有 trampoline、日志、路径、教师端判断；把 Core 从 `bin\core_cache\core.<pid>.<n>.dll` 的缓存副本加载，源文件永远不被映射 |
| Core | `bin\bypass_hook_core.dll` | `Hook_*` 实现、窗口/抓屏/关机守卫；由 Loader `LoadLibraryEx` 后调 `Core_Init` |

**什么时候要重编什么**（很重要）：

| 改动 | 重编 | 重启课堂客户端 |
|---|---|---|
| 改 `Hook_*` 的拦截逻辑 | Core | 否 —— 用「重载 Core」热换 |
| 加一个新的拦截点（新的 inline hook / IAT dispatcher） | Loader + Core | 是（Loader 占着 DLL 名，不能热换） |
| 只改主界面 / 托盘 / 文档 | `bypass.exe` | 否 |

#### 热重载 Core

主界面底部「**重载 Core**」按钮，或托盘右键 →「重载 Core（热更新）」。它走
命名管道 `\\.\pipe\bypass_core_ctl_<pid>`：

- **目标进程**：按已知课堂进程名（`RELOAD_TARGETS`：`Student.exe`、`MultiClient.exe`、
  `MultiRender.exe`、`ScreenRender.exe`、`ScreenRecord.exe`、`SharedDesktop.exe`、
  `StudentScreenHelper.exe`、`ScreenPen.exe`、`AudioRender.exe`、`AudioSender.exe`）
  逐个找 PID 再连管道；不做全进程扫描。连不上 = 没注入或旧 DLL，记一行日志跳过。
- **超时**：Loader 最坏约 12 秒（两个 3 秒线程等待 + VD 3 秒 + 3 秒 drain），所以
  界面不等：请求发出后马上返回，后台线程最多等 15 秒，3 秒还没结果就把状态栏切成
  「reloading Core (up to 15s)」。**只有 Loader 明确回 `err` 才算失败**。
- **结果分类**（日志里逐进程一行）：

  | 响应 | 含义 | 状态栏 |
  |---|---|---|
  | `ok seq=N cache=...` | 新 Core 已加载 | `Core reloaded` |
  | `rolledback seq=N` | 新 Core 失败，已回滚到旧 Core | `reload failed, old Core kept` |
  | `err refused-by-core` | 阶段 1 拒绝（还有 hook 在跑 / 线程还在停），什么都没动 | `nothing reloaded` |
  | 其他 `err` | 失败，什么都没动（`--no-rollback` 时可能只剩半套） | `nothing reloaded` |
  | `err hooks-busy (core disabled)` / `err coreDead` | **进程半禁用，终态** | `Core disabled, restart the process` |

#### 版本自检：内容哈希，不再看链接时间

「检查当前状态」原来比 PE `TimeDateStamp`（链接时间），同秒重建 / 可重复构建 /
手工改过的 DLL 都会误报「current」。现在优先用**内容哈希**：

- Loader 加载时对自己映射的那份 cache 副本算 FNV-1a 32，`status` 里回 `hash=xxxxxxxx`；
- 主程序算 `bin\bypass_hook_core.dll` 的哈希比较，**相等才算 current**；
- 结论分三档：**Core 旧 → 点「重载 Core」**（不用重启）；**Loader 旧 / 半禁用 →
  重启课堂客户端**；**磁盘文件正在被构建重写 → 提示稍后再查**；
- 没有控制管道（旧单 DLL）或 Loader 正在 reload（管道被占）时，退回 PE stamp 比较。

> 顺带修掉一个真 bug：从 64 位主程序枚举 32 位课堂进程的模块必须带
> `TH32CS_SNAPMODULE32`，否则模块表永远为空、每个进程都显示 `not loaded`。

#### 多开 / 并发

控制管道是**单连接**的，因此第二个客户端（UI + 脚本、或重载期间的检查）会拿到
`ERROR_PIPE_BUSY`：这不是「没注入」，会等管道实例空出来（`WaitNamedPipe`）再发。

一次「重载 Core」会逐个进程发送，4 个课堂进程同时开着也是 `ok=4`；每个进程用自己的
`core.<pid>.<seq>.dll` 缓存副本、自己的 `g_inHookCount` / UEF 栈 / blob，互不影响。
一个进程崩溃（`!! crash`）只影响它自己，其余仍然正常。

> 旧 Core 的初始化（`Sleep(200)` 之后才建 VD / caption 线程）与 reload 撞车时，
> 会出现「刚建好的线程不停」→ 阶段 2 超时。已用 `g_initDepth` 握手修掉（见
> `docs\design.md` 2.9「多实例与并发」），`tests\multi_test.ps1` 覆盖。

#### 半禁用不是「已直通」

`before_unload` 返回 1 之后才置 `g_unloading=1`，然后 Loader 做权威 drain（3 秒）。
如果这时还有调用卡在 hook 里（Core 撒谎：返回 1 却没停线程），Loader 保持 `g_unloading=1`、
置 `g_coreDead=1`、**不卸载**，进程就停在半禁用状态。33 个 dispatcher 里：

| 类别 | 数量 | disabled 期行为 |
|---|---|---|
| 11 个 `HOOK_ENTER` 包装 | 11 | 直通 |
| 6 个 inline hook（`ShowWindowAsync`、`CreateProcessA`、`WinExec`、`ExitWindowsEx`、`ShellExecuteA/W`） | 6 | 直通 |
| 窗口状态/焦点：`SetWindowPlacement`、`CreateWindowExW`、`SetForegroundWindow`、`BringWindowToTop`、`SetActiveWindow` | 5 | 直通（`CreateWindowExW` 只保留隐藏 ShutDownDlg 这条关机安全逻辑） |
| 抓屏守卫：`BitBlt`、`CreateDCW`、`CreateDCA`、`GetDC`、`GetWindowDC` | 5 | 跑完整逻辑 |
| 关机入口：`InitiateSystemShutdownA/W/ExA/ExW` | 4 | 跑完整逻辑（关机守卫开时仍拦） |
| 输入锁：`SetWindowsHookExA/W`（拦 `WH_MOUSE_LL`/`WH_KEYBOARD_LL`） | 2 | 跑完整逻辑（输入不被锁） |

所以 disabled 期**只剩安全类 hook 在跑**：抓屏冻结 + 关机入口拦截 + 低层输入钩子抑制，
其余全部直通。「重载失败」不等于「hook 停了」：窗口显示/焦点/摆放、进程创建、外壳、
`ExitWindowsEx` 都直通了，但抓屏冻结仍在、`InitiateSystemShutdown*` 仍被拦、
输入锁仍被抑制。日志会打 `process disabled (partially bypassed)` + 具体说明。
`g_coreDead` 是终态（进程全局、没有重置路径），只能**重启该课堂进程**或重新注入。

> `SetWindowsHookExA/W` 故意不放通：它挡的是客户端锁键盘/鼠标的通道，属于安全类；
> disabled 期放通等于可能被锁输入，比“点窗口没反应”严重。

> 那 11 个不进 `g_coreDisabled` 检查的入口都只读模块全局 / `bypass_shut.ini`，不碰
> `StateClose` 释放过的 `g_stateView` / `g_stateMap` / `g_stateEvent`（机械检查覆盖）。
> 保留它们是为了在「已经半废」的状态下多留一层保护（抓屏冻结 + 关机拦截）——
> 这两类都不会重入 Core，也不会访问被释放的资源。

> 8 个 inline hook 的直通不变式中，只有 `WinExec` 有自动测试（`tests\reentry_test.ps1`）；
> 其余 5 个 inline hook 和 7 个窗口状态/焦点 hook 靠代码审查（难构造零副作用探针）。
> 改这些入口时必须确认 `CORE_DISABLED()` 仍在函数最前面。

> `before_unload` 里**不调用任何 hook 函数**——它只用 `real_SetWindowLongPtrW`、
> `StateClose` 这类裸 Win32，所以上面这张表不影响卸载时序。

> 另外一条不对称：Loader 故意**不 patch Core 模块自己的 IAT**。Core 内部调 IAT-only
> 函数直接走原函数（不会重入 dispatcher），只有 8 个 inline hook 会重入。

#### 回退

`bin\bypass_hook_single.dll` 是拆分前那版的完整 DLL（Loader + Core 合一）。把它复制成
`bin\bypass_hook32.dll`（覆盖 Loader），**重启课堂客户端**，再打开主程序。
只换文件不重启无效——旧 DLL 已经在进程里；`inject32` 看到
`bypass_hook32.dll` 或 `bypass_hook_single.dll` 任一个已加载都会跳过。

## 6. 安全与回退

- 误锁键盘：点主界面「解锁键盘」。键盘被驱动锁住时**快捷键按不出来**；
  鼠标被冻结先按 `Ctrl+Alt+E` 应急点击，再点按钮。
- 卡在 `NetTeachingDesktop`：`Ctrl+Alt+D`。
- 恢复原生全屏广播：退出工具，在 `bin\` 放 `bypass_hook.off`，重启课堂软件。
- 让广播窗重新跟着当前虚拟桌面：`bin\bypass_vd.ini` 的 `enabled=0`（不用重启课堂软件）。
- 「重载 Core」提示 **Core disabled, restart the process**：该课堂进程已半禁用（终态，
  见 5.6），hook 只剩部分拦截，必须**重启那个课堂进程**（或让 `bypass.exe` 重新注入）。

## 7. 文档与签名

| 文档 | 内容 |
|---|---|
| [`docs/how-to-use.md`](docs/how-to-use.md) | 部署、课堂流程、快捷键、故障对照 |
| [`docs/design.md`](docs/design.md) | 原理、钩子、窗口行为、日志、已知限制 |
| [`lpe/README.md`](lpe/README.md) | 自动提权链路与限制 |
| [`build/README.md`](build/README.md) | 编译脚本、自签证书、签名与打包 |

签名可选（自签只适合内网，目标机要装证书才信任），完整命令见 `build\README.md`：

```bat
powershell -ExecutionPolicy Bypass -File build\make-selfsigned-cert.ps1 -Trust
build\sign.bat local\certs\bypass_tool-test.pfx
```
