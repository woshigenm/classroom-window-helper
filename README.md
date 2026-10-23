# 噢易多媒体网络教室 V10.8 —— 学生端「小窗化广播」工具

把教师端的**全屏广播**变成学生端一个**可拖动 / 可缩放 / 可最大化 / 可最小化到任务栏**的普通窗口，
并尽量不影响课堂软件的其它功能。实现原理见 [`docs/design.md`](docs/design.md)。

> 上手步骤和快捷键见 [`docs/how-to-use.md`](docs/how-to-use.md)。

---

## 1. 交付文件（在 `bin\`，必须放在同一个目录）

| 文件 | 位数 | 作用 |
|---|---|---|
| `bin\bypass.exe` | 64 位 | 主程序：界面、托盘、看门狗、注入调度 |
| `bin\bypass_hook32.dll` | 32 位 | 注入到 Student / MultiClient / ScreenRender 的钩子 |
| `bin\inject32.exe` | 32 位 | 注入器（由主程序拉起，也可手动运行） |

当前构建：`bin\bypass.exe` 250,920 字节、`bin\bypass_hook32.dll` 143,192 字节、`bin\inject32.exe` 78,112 字节（2026-10-23 19:00，已用本机自签证书签名）。

| 文件 | 大小 | MD5 |
|---|---|---|
| `bin\bypass.exe` | 250,920 | `5DE8DC6FDDD2C885363E7D49EBD7A0FC` |
| `bin\bypass_hook32.dll` | 143,192 | `FC4DF87402020BBEE92E045F221FF5F1` |
| `bin\inject32.exe` | 78,112 | `B4F3DC1209093949E95C76672BAEBBD7` |

> ⚠ `bin\` 必须在本机磁盘（不要放 VMware 共享目录 / 网络盘）：
> HGFS 不支持 PE image section 映射，目标进程里 `LoadLibraryW` 会返回 0，注入必失败。

## 2. 快速开始

1. 把整个仓库复制到本机（如 `C:\bypass_tool\`）；没有 `bin\` 就先按第 4 节编译。
2. 把 `bin\` 加入 Windows Defender 排除项（否则注入的 DLL 可能被拦截）。
3. 以管理员身份运行 `bin\bypass.exe`；托盘出现图标即启动。
4. 教师端开始广播时，学生端会自动把广播窗降为小窗。
5. 退出：托盘菜单「退出」或 `Ctrl+Alt+Q`。

完整流程（含教师机 / 学生 VM、开关文件、故障处理）见
[`docs/how-to-use.md`](docs/how-to-use.md)。

## 3. 目录结构

```
bypass_tool\
├─ .gitignore
├─ README.md                  本文件
├─ Makefile                   GNU Make：主程序 + DLL + 注入器
├─ make.bat                   临时加工具链 PATH 后调用 mingw32-make（免配环境）
├─ bin\                       所有编译产物（gitignore；运行期日志/开关也在这里）
│   ├─ bypass.exe
│   ├─ bypass_hook32.dll
│   ├─ inject32.exe
│   ├─ hooktest32.exe
│   ├─ bypass_config.ini      运行配置（自动生成）
│   └─ bypass_hook.log        DLL 运行日志（自动生成）
├─ src\                       交付组件源码
│   ├─ README.md              模块 / 线程与锁 / 关键不变量
│   ├─ bypass.c               主程序
│   ├─ bypass_hook.c          注入 DLL
│   ├─ inject32.c             注入器
│   └─ bypass_state.h         主程序 / DLL 共享状态（配置 + 共享内存）
├─ tests\                     自测
│   ├─ hooktest32.c           Hook 自测载体源码
│   └─ hooktest_result.txt    一次实测输出（参考）
├─ build\                     编译脚本
│   ├─ Makefile               转发到根 Makefile（可在 build\ 里直接 make）
│   ├─ make.bat               转发到根 make.bat（自动加工具链 PATH）
│   ├─ build_bypass.bat       编译 bypass.exe
│   ├─ build_hook32.bat       编译 DLL + inject32.exe
│   ├─ sign.bat               cmd 包装：调用 sign.ps1 并提示密码
│   ├─ sign.ps1               用 PFX 签名 bin\ 三个交付文件
│   ├─ make-selfsigned-cert.ps1  生成内网测试用自签证书
│   ├─ package.ps1            打包 dist\ 下的 Release zip
│   └─ README.md              编译 / 签名 / 打包完整说明
├─ docs\
│   ├─ how-to-use.md          使用手册（部署 / 上课 / 快捷键）
│   └─ design.md              设计与实现（钩子 / 窗口 / 日志 / 限制）
```

## 4. 编译

所有产物统一输出到 `bin\`。两种方式等价：

```bat
:: 64 位主程序（先退出正在运行的 bin\bypass.exe）
build\build_bypass.bat

:: 32 位 DLL + 注入器（先关闭 Student / MultiClient / ScreenRender）
build\build_hook32.bat
```

```bat
:: 推荐：make.bat 自己把两个工具链加进 PATH，不用改系统环境
make.bat                  :: 主程序 + DLL + 注入器
make.bat bypass           :: 只编译主程序
make.bat hook32           :: 只编译 DLL + 注入器
make.bat test             :: 自测载体 bin\hooktest32.exe
make.bat check            :: 打印两个编译器版本
make.bat rebuild          :: clean + all
make.bat clean            :: 删除 bin\ 下的产物（⚠ 包括三个交付文件）
```

`make.bat` 用 `setlocal` 临时改 PATH，退出即还原。额外参数原样传给 make，
例如 `make.bat -n all`、`make.bat -B hook32`。双击运行（不带参数）时会在结束前
暂停等按键，方便看结果；命令行带参数调用不会暂停。

如果坚持自己调 `mingw32-make`，先手动加 PATH：

```bat
set PATH=C:\Program Files\RedPanda-Cpp\mingw64\bin;%PATH%
mingw32-make all
```

`build\` 目录下还有 `make.bat`（转发到根）和 `Makefile`（转发到根 `Makefile`），
在 `build\` 里直接 `make.bat` 效果相同。

工具链：

| 目标 | 编译器 |
|---|---|
| 64 位 | `C:\Program Files\RedPanda-Cpp\mingw64\bin\gcc.exe` (11.5.0) |
| 32 位 | `C:\mingw32\bin\i686-w64-mingw32-gcc.exe` (16.2.0) |

32 位 DLL 必须 `-static -static-libgcc`（脚本已带），否则目标进程里 `LoadLibraryW` 返回 0。

> `bin\` 在 `.gitignore` 里：`git clone` 后请先编译。要把三件套随仓库发布，
> 用 GitHub Release 附件，或 `git add -f bin/bypass.exe bin/bypass_hook32.dll bin/inject32.exe`。

## 5. 运行期配置与安全阀

持久配置在 `bin\bypass_config.ini`（主程序自动生成，手工保存后 1 秒内生效）：

| 键 | 作用 |
|---|---|
| `window_mode` | `1`（默认）= 窗口模式；`0` = 关闭 |
| `title_autohide` | `1`（默认）= 最大化后自动隐藏标题栏；`0` = 关闭 |
| `minimize_mode` | `taskbar`（默认，真最小化）/ `tray`（收起不占任务栏） |
| `small` | 空/`0` = 默认；`0.35` = 屏宽比例；`640x360` = 固定外框 |

运行期通过共享内存 + 事件同步；“强制小窗 / 恢复被收起的窗口”是内部计数器，不再需要手工建文件。

| 安全阀 | 作用 |
|---|---|
| `bin\bypass_hook.off` | 存在 = 全部放行（暂停钩子）；退出工具不会写，启动时自动清理 |

## 6. 安全与回退

- 误锁键鼠：`Ctrl+Alt+K` 解锁键盘（工具内还有鼠标逃生通道）。
- 卡在 `NetTeachingDesktop`：按 `Ctrl+Alt+D`（主程序的桌面守护）。
- 想恢复原生全屏广播：退出工具，并在 `bin\` 放 `bypass_hook.off`，再重启课堂软件。

## 7. 文档索引

| 文档 | 内容 |
|---|---|
| [`docs/how-to-use.md`](docs/how-to-use.md) | 部署、课堂流程、快捷键、最小化/标题栏行为、故障排查 |
| [`docs/design.md`](docs/design.md) | 原理、钩子、窗口行为、日志与已知限制 |
| [`build/README.md`](build/README.md) | 编译、签名、自签证书与目标机信任步骤 |

## 8. 代码签名（可选）

正式分发建议 OV/EV 证书或 Azure Trusted Signing；自签名只适合内网测试，目标机要安装证书才会信任。
完整命令见 [`build/README.md`](build/README.md)。

```bat
:: 1) 生成自签测试证书（私钥只落在 local\certs，已 gitignore）
powershell -ExecutionPolicy Bypass -File build\make-selfsigned-cert.ps1 -Trust

:: 2) 签名 bin\ 下的三个交付文件（会提示输入 PFX 密码）
build\sign.bat local\certs\bypass_tool-test.pfx
```

PowerShell 一行写法（适合自动化）：

```powershell
powershell -ExecutionPolicy Bypass -File build\sign.ps1 -Pfx local\certs\bypass_tool-test.pfx -Password "your-password"
```

- 在 cmd 里**不要用反引号换行**；要么写一行，要么用 `build\sign.bat`。
- 有正式 PFX 时把路径/密码换成自己的。
- **不要把 PFX / 私钥提交到仓库**；`.gitignore` 已排除 `*.pfx` / `*.cer` / `*.key`。
- 自签名后 `Get-AuthenticodeSignature` 可能显示 `UnknownError`（本机未信任），不是签名失败；目标机导入 `.cer` 后才显示 `Valid`。
- 签名能改善 SmartScreen / 部分 AV 信誉，但**不能保证**不被 `Trojan:Win32/Wacatac.B!ml` 这类 ML 误报命中。
