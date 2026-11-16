# 噢易多媒体网络教室 V10.8 —— 学生端「小窗化广播」工具

把教师端的**全屏广播**变成学生端**可拖动 / 可缩放 / 可最大化**的普通窗口，
并默认把广播窗钉在一号虚拟桌面（切到别的桌面后再广播，不会被拽回去）。

原理见 [`docs/design.md`](docs/design.md)，上手 / 快捷键 / 故障排查见 [`docs/how-to-use.md`](docs/how-to-use.md)。

## 1. 交付文件（都在 `bin\`，必须同一目录）

| 文件 | 作用 |
|---|---|
| `bin\bypass.exe` | 主程序：界面、托盘、看门狗、注入调度 |
| `bin\bypass_hook32.dll` | 注入课堂进程的钩子（32 位） |
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
make.bat clean | rebuild | check
```

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

## 6. 安全与回退

- 误锁键盘：点主界面「解锁键盘」。键盘被驱动锁住时**快捷键按不出来**；
  鼠标被冻结先按 `Ctrl+Alt+E` 应急点击，再点按钮。
- 卡在 `NetTeachingDesktop`：`Ctrl+Alt+D`。
- 恢复原生全屏广播：退出工具，在 `bin\` 放 `bypass_hook.off`，重启课堂软件。
- 让广播窗重新跟着当前虚拟桌面：`bin\bypass_vd.ini` 的 `enabled=0`（不用重启课堂软件）。

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
