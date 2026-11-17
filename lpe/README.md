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

## 运行时文件（不提交，不打包）

| 路径 | 用途 | 生命周期 |
|---|---|---|
| `%LOCALAPPDATA%\ClassroomBypass-LPE\<runid>\` | 本次运行的 `wallet.db`、payload 副本、`result.txt` | 保留供排查，手工删除 |
| `%LOCALAPPDATA%\ClassroomBypass-LPE\documents.orig` | Documents 原值快照 | 重定向前写入，恢复成功后删除 |
| `%LOCALAPPDATA%\ClassroomBypass-LPE\abort.<elevate-pid>` | “跳过”信号：`elevate.c` 写一行，`trigger.ps1` 的等待循环看到它就收尾（设计见 `docs\P1-abort.md`） | 写方自己删：启动前清残留，退出前删掉 |

放在用户目录而不是安装目录，是因为安装目录可能只读（Program Files），而这些文件
必须能写；同时它们也不应该跟着发布包发给别人。

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
- 「跳过」是协作退出，不是直接杀进程：`elevate.c` 写 `abort.<pid>`，脚本在 200 ms 轮询里
  看到它就 `return`，由 `finally` 把 Documents 写回；3 秒没有反应才 `TerminateProcess`
  （见 `docs\P1-abort.md`）。所以「跳过」只保证**尝试**恢复，不保证回到用户最初配置的路径。
- 脚本在 Job Object（`KILL_ON_JOB_CLOSE`）里，启动器被强杀时它会跟着死；正常返回前会先
  撤掉这个限制，否则成功路径会把还在跑 `finally` 的脚本一起杀掉。
- 「跳过」通道**目前只有单元覆盖，未做端到端实测**（需要一次真实 UAC 交互）。若用户反馈
  「点了跳过但 Documents 没回来」，先按 `docs\P1-abort.md` §4 的三处失败点查这条链路，
  不要先怀疑快照。
- `SHSetKnownFolderPath` **失败时也会写入**：传一个不存在的目录，它先把
  `User Shell Folders\Personal` 和 `Shell Folders\Personal` 都改成那个路径，再返回错误，
  **不回滚**（实测：`set C:\__no_such_dir__` 退出码 1，但两处注册表值已经变坏，
  随后 `get` 报 `0x80070002`）。所以 `trigger.ps1` 每次 `set` 之前都用
  `Test-Path -PathType Container` 验目标，验不过就 `exit 4`，而不是照着写——
  写进去就是一个指不到任何目录的 Documents。
- stale 自愈分两级：**快照**优先，**候选列表**只是兜底。
  - 快照 `%LOCALAPPDATA%\ClassroomBypass-LPE\documents.orig`：每次重定向前把当前值
    写进去（UTF-8 无 BOM，中文用户名不会坏），恢复成功才删除。放用户目录而不是
    `bin\lpe\`，因为安装目录可能只读（Program Files），而快照必须能写；它也不该
    跟发布包走。读回时会验格式（非空、单行、≤260、绝对路径），验不过就当作“没有
    记录”并告警；验过但目录当前不可达 → 见下一条。
  - 候选列表：只有没有快照时（旧版本遗留的坏状态）才用，顺序
    `$env:OneDrive` → `$env:OneDriveConsumer` → `%USERPROFILE%\Documents`。
    OneDrive 优先，是因为 KFM 用户通常还留着一个空壳 `%USERPROFILE%\Documents`，
    写进去会悄悄取消同步（看起来“修好了”，其实换了目录）。
  - 两者都不可用 → `exit 4`，退回 UAC，并提示手工
    `bin\lpe\documents_path.exe set <真实路径>`。快照存在但当前不可达
    （移动硬盘拔了、共享离线）时同样拒绝，而不是拿候选覆盖它——那是最后一版真值。
    空值分支和 LPE 分支对这三种状态的处置一致。
  - 写快照失败（磁盘满、权限）只写 `WARNING` 并继续：本次运行内 `finally` 仍然有效，
    只有“硬杀”才需要跨运行记录；`exit 4` 会让只读安装的用户完全用不了。
- 回调 DLL 的服务名是 `CVE49176_<pid>_<tick>`，每次运行唯一，所以不存在
  “上次残留 → 本次 `ERROR_SERVICE_EXISTS`”。但**残留窗口是真实存在的**：
  `CreateServiceW`（`wallet_callback_shell.c:121`）与 `DeleteService`（:137）之间被杀，
  服务会留下。

  风险范围要说准：`SERVICE_DEMAND_START` 不会自启；`CreateServiceW` 没有传
  `SECURITY_ATTRIBUTES`，服务用系统默认 DACL，Interactive User 只有
  `CCDCLCSWLOCRRC`（查询/枚举），**没有 RP**，所以普通权限下 `sc start` /
  `sc stop` / `sc delete` 都是 Access Denied。能触发的门槛是 **High 及以上**：
  用户之后任何一次 UAC 提权（包括本工具再跑一次，或别的程序）都能让它以 SYSTEM
  加载 `%LOCALAPPDATA%\ClassroomBypass-LPE\<runid>\payload\wallet_callback_shell.dll`。
  那个 DLL 在用户可写目录里，所以这仍然是持久提权面——只是入口不是“现在随便一个
  普通进程”，而是“下一次高完整性上下文”。

  `trigger.ps1` 启动时只读扫描 `CVE49176_*`，命中才写 `WARNING`（不自动删：
  删服务需要本脚本不一定持有的权限）。

  **先跑 `build\cleanup_lpe.ps1`（仓库内脚本，发布包不含 `build\`；无参数 = 只列不删）**：它把服务、run 目录、
  `documents.orig`、`abort.*` 一起列出来，并给每一项标 `[RUNNING, do not delete]`
  或 `[ORPHAN]`。确认没有在跑的实例后，再按需删：`-RunId <32hex>` / `-Service <name>`
  / 管理员下 `-All -Force`（要求 `net session` 通过并输入 `DELETE`）。

  两个约定和直觉不同，脚本的注释里也写了：

  - 服务名里的 pid 是**钱包服务（WalletService）的 pid**，不是提权实例的 pid，
    所以它几乎总是“活着”，不能拿来判活。存活判断看服务状态 + run 目录里的进程。
  - `documents.orig` 任何参数下都不删：它是用户真实 Documents 路径的唯一副本，
    删掉等于把可恢复状态变成永久损坏。

  没有脚本（或脚本跑不起来）时的手工序列——**整段必须在管理员上下文执行**。
  普通权限下前三条都会失败，而失败后继续 `rmdir` 会变成
  “DLL 已删、服务还在”，比不修更糟（服务指向不存在的文件，`sc start` 会走 DLL
  搜索顺序）：

  ```
  rem 先确认真的是管理员；不是就别往下走
  net session >nul 2>&1 || (echo NOT ELEVATED & goto :eof)

  sc query  CVE49176_<pid>_<tick>
  sc stop   CVE49176_<pid>_<tick>
  sc delete CVE49176_<pid>_<tick>
  rem 只有 sc delete 成功了才删目录
  rmdir /s /q "%LOCALAPPDATA%\ClassroomBypass-LPE\<runid>"
  ```

  顺序不能反：先删目录再 `sc delete`，服务会有一段指向不存在文件的窗口。
  （写在这里是为了挡住“先删目录更直观”的改法。）
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
