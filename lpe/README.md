# lpe\ —— 启动时的自动提权载荷

这个目录只解决一件事：**把 `bin\bypass.exe` 以管理员身份拉起来**。
工具本身仍然需要一个管理员进程（驱动 IOCTL + 注入课堂进程），老的流程是弹 UAC；
这里额外提供一条“不需要用户点 UAC”的路，并且**失败就退回 UAC / 手动**。

## 文件编码约定（改代码前必读）

| 文件 | 编码 | 谁写 | 谁读 |
|---|---|---|---|
| `payload\result.txt` | **纯 ASCII 追加** | 回调 DLL（原样）+ `shell_broker.exe`（事件行 `event=...` + 诊断行 `note=...`） | `trigger.ps1`（默认编码 + 正则） |
| `payload\command.txt` | UTF-16LE + BOM | `trigger.ps1`（来自 `-TargetCommand` 参数） | 只被 `shell_broker.exe` 读 |
| `bin\bypass_hook.log` | ACP（中文系统 = GBK） | 启动器 / `trigger.ps1`（`[elevate]` 进度行）/ broker 拉起的实例 / 注入的 DLL | 人 + 启动器的握手扫描 |

`result.txt` 里混进任何非 ASCII 字节 = 集成版必挂（原版 PoC 两边都是 ASCII 所以能跑）。

## 文件

| 路径 | 说明 |
|---|---|
| `trigger.ps1` | 本次的入口。构造并触发 WalletService 链路，结束后无条件还原 Documents 路径 |
| `payload\wallet_ese_seed.exe` | 造带持久化回调的 `wallet.db`（上游 MSVC 构建，源码见 `src\`） |
| `payload\wallet_callback_shell.dll` | 被 WalletService 以 SYSTEM 加载，创建按需服务（同上） |
| `payload\documents_path.exe` | 读写 `FOLDERID_Documents`（同上） |
| `payload\README.txt` | 来源、SHA256、与原版的差异说明 |
| `src\*.c` | 上面三个二进制的源码，可直接修改；`make.bat lpepayload` 重新编译 |
| `..\src\lpe_broker.c` | 由本仓库编译成 `bin\lpe\shell_broker.exe`：拉起 `bin\bypass.exe` |

构建：`make.bat lpe`（`make.bat all` 也会带上）。产物全部落到 `bin\lpe\`，
**运行期只读 `bin\lpe\`**，本目录不参与运行。

`payload\` 里三个二进制默认是上游 MSVC 构建。改了 `src\` 的源码后，先用
`make.bat lpepayload` 重编（mingw-w64），再 `make.bat lpe` 铺到 `bin\lpe\`。
重编会改变 SHA256，而 `payload\README.txt` 记的是原值。细节见 `src\README.md`。

`make.bat lpe` 会先用 `build\check_payload.ps1` 比对 `src\*.c` 的内容哈希与
`payload\SOURCES.sha256`：源码改过而没重编时直接失败，不会静默把旧二进制拷进 `bin\lpe\`。

## 调用链

```
bin\bypass.exe（普通用户，非管理员）
  └─ powershell -EncodedCommand <trigger.ps1 + TargetCommand>
       ├─ seed/Documents 重定向 + WalletManager.RequestStoreAsync()
       ├─ WalletService(LocalSystem) 加载 wallet_callback_shell.dll
       ├─ 该 DLL 创建按需服务 -> shell_broker.exe <result.txt>
       ├─ broker 读 command.txt（= "bin\bypass.exe ..."）
       └─ CreateProcessAsUserW(winsta0\default) -> 管理员身份的 bypass.exe
            └─ 往 bin\bypass_hook.log 写握手行（elevated instance pid=... is up）
```

启动器（`src\elevate.c`）扫共享日志的尾部找握手行，**不等退出码**：退出码只能证明“服务跑过”，
握手行才能证明“我们的程序真的以管理员身份在跑”。

## 安全性 / 限制

- 只启动 `command.txt` 里那一行命令，而那是启动器编进 `-EncodedCommand` 的 `bin\bypass.exe` 路径。
- 优先取会话令牌的**未过滤链接令牌**（真正的管理员组身份），拿不到才退回 SYSTEM（PoC 原行为）。
- `trigger.ps1` 的 `finally` 会恢复 Documents 已知文件夹。
- `SHSetKnownFolderPath` **失败时也会写入**：传一个不存在的目录，它先把
  `User Shell Folders\Personal` 和 `Shell Folders\Personal` 都改成那个路径，再返回错误，
  **不回滚**（实测：`set C:\__no_such_dir__` 退出码 1，但两处注册表值已经变坏，
  随后 `get` 报 `0x80070002`）。所以 `trigger.ps1` 每次 `set` 之前都用
  `Test-Path -PathType Container` 验目标，验不过就 `exit 4`，而不是照着写——
  写进去就是一个指不到任何目录的 Documents。
- stale 自愈分两级：**快照**优先，**候选列表**只是兜底。
  - 快照 `bin\lpe\documents.orig`：每次重定向前把当前值写进去（UTF-8 无 BOM，
    中文用户名不会坏），恢复成功才删除。脚本被杀时它留在磁盘上，下一次运行直接
    读回真值（`Test-Path -PathType Container` 验过才用），所以自定义重定向
    （`D:\MyDocs`、网络盘）也能自愈。
  - 候选列表：只有没有快照时（旧版本遗留的坏状态）才用，顺序
    `$env:OneDrive` → `$env:OneDriveConsumer` → `%USERPROFILE%\Documents`。
    OneDrive 优先，是因为 KFM 用户通常还留着一个空壳 `%USERPROFILE%\Documents`，
    写进去会悄悄取消同步（看起来“修好了”，其实换了目录）。
  - 两者都不可用 → `exit 4`，退回 UAC，并提示手工
    `bin\lpe\documents_path.exe set <真实路径>`。快照存在但当前不可达
    （移动硬盘拔了、共享离线）时同样拒绝，而不是拿候选覆盖它——那是最后一版真值。
- 回调 DLL 的服务名是 `CVE49176_<pid>_<tick>`，每次运行唯一，`StartService` 之后
  立即 `DeleteService`，所以没有“上次残留 → 本次 `ERROR_SERVICE_EXISTS`”这条路径。
  `trigger.ps1` 启动时仍对 `CVE49176_*` 做一次只读扫描，发现残留才写 `WARNING`
  （不自动删除：删服务需要本脚本不一定持有的权限）。
- `documents_path.exe get` 的输出会丢非 ASCII 字符（CRT `"C"` locale），所以
  `trigger.ps1` 只用它写日志，不再用它做任何判断：读当前路径走 .NET 的
  `[Environment]::GetFolderPath`，判断成败看 `documents_path.exe` 的退出码。
- 漏洞在 **2026 年 7 月**修复；补丁机上必然失败，此时自动走 UAC → 手动提示。
- 只用于自己的机器 / 自己的课堂环境。分发前请确认当地法律与学校规定。

## 手动自测

```bat
rem 1) 先确认不是管理员（普通 PowerShell 里执行）
rem 2) 直接跑 trigger（等价于 bypass.exe 自动提权）
powershell -NoProfile -ExecutionPolicy Bypass ^
  -File bin\lpe\trigger.ps1 ^
  -TargetCommand '"C:\classroom-window-helper\bin\bypass.exe"' -TimeoutSec 45

rem 3) 只想看进度/报错（全部都在共享日志里）：
type bin\bypass_hook.log

rem 结果：
rem   bin\bypass_hook.log 里有 elevated instance pid=... is up = 已经以管理员在跑
rem   %LOCALAPPDATA%\ClassroomBypass-LPE\<runid>\payload\result.txt  PoC 的原始事件
```

> 直接 `.\trigger.ps1` 会被执行策略拦住——这是 PowerShell 对**脚本文件**的限制。
> `bypass.exe` 用的是 `-EncodedCommand`（内容 Base64 后执行），不经过脚本文件，所以
> 即使 `Get-ExecutionPolicy` 是 `Restricted` 或被组策略锁死也能跑。手工执行时加
> `-ExecutionPolicy Bypass -File` 即可。
