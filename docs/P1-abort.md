# P1b 设计：点「跳过」时的协作退出

状态：**草案，未实现**。审完这份再动 `src\elevate.c`。

## 1. 先摆事实（读代码得出，不是推测）

```
bypass.exe（普通用户；elevate.c）
 └─ powershell.exe -EncodedCommand <trigger.ps1>      ← RunHidden(), elevate.c:265
       └─ （conhost.exe 之类）
(WinRT)  WalletService   [服务，不在我们的进程树里]
 └─ shell_broker.exe
      └─ bypass.exe（提权实例）                        ← broker 的 CreateProcessAsUser
```

由此得出三条，其中第一条推翻我上一轮的说法：

1. **`powershell.exe` 是 `elevate.c` 的直接子进程**，不是提权实例的子进程。上一轮我说
   “elevated bypass.exe 的孙进程是 powershell”，方向错了：提权实例是 broker 拉起来的，
   和我们的树没有父子关系。
2. 持有 Documents 重定向、以及那个 `finally` 的，是 **powershell（跑 trigger.ps1 的那个）**。
   所以 P1b 要保护/必须杀干净的，就是它。
3. 提权实例 `bypass.exe` **不在 job 里，也不该被「跳过」杀掉**——它是用户本来要的东西
   （握手行已出现时说明它已经在跑）。

结论：Job Object 的作用范围是 **powershell(+conhost)**，不是“整棵树”。这已经够用，
因为它保证 `elevate.c` 退出后不会留下一个还持有 Documents 重定向的孤儿。

## 2. 现在的缺口

`g_swSkip` → `break` → `TerminateProcess(proc, 1)`（`elevate.c:859-861`），正好跨过
trigger.ps1 的 `finally`：

- Documents 停在 `$root`（本机自愈靠下一次运行的快照分支，但“每次跳过都必现”）；
- `CreateServiceW` 与 `DeleteService` 之间的窗口如果正好被撞上，服务会残留
  （见 `lpe\README.md` 的清理指引）；
- 日志里没有任何“用户点了跳过”的记录，和“脚本自己失败了”无法区分。

## 3. 顺序

1. **快照**（已完成）——P1b 的安全网，没有它，“杀得干净”反而变成新的必现故障；
2. **abort 协作协议**——让 `finally` 有机会跑，恢复 Documents；
3. **Job Object**——abort 没用时的最后手段；
4. **清理与文档**。

## 4. abort 的语义：B（声明 + 等退出 + 超时硬杀）

| 步骤 | `elevate.c` | `trigger.ps1` | `proc` |
|---|---|---|---|
| 1 | 用户点跳过 → 写 `abort.<pid>`（一行 `abort <tick>`） | 仍在 `do{}`；下一次 200ms 轮询看到文件 | 存活 |
| 2 | `WaitForSingleObject(proc, 3000)` | `throw` / `break` 出循环 → `finally` 里 `set $original` | 存活，正常退出 |
| 3a | 等到了 → 关闭句柄，继续走 UAC / 返回 | 已恢复到原值 | 退出码 0 |
| 3b | 超时 → `TerminateProcess(proc, 1)`，再关 job | 被硬杀，`finally` 没跑 | 死 |

为什么不是 A（写文件后直接放弃等待）：powershell 会继续把 LPE 跑完、把 Documents
`set` 回去，但调用方已经走了，没人知道结果；下一次运行会再试一遍 LPE，用户以为自己
取消了。B 让“跳过”变成一次有结果的事务。

**「跳过」的边界**：`elevate.c` 不再关心结果，但**提权实例不受影响**。

| 组件 | 点跳过之后 |
|---|---|
| `trigger.ps1` | 看到 abort → 退出 `do{}` → `finally` 恢复 Documents → 退出 |
| `elevate.c` | 写 abort → 等 powershell 退出（≤3000ms）→ 超时硬杀 → 继续走 UAC / 返回 |
| 提权实例（broker 拉起） | 不受影响，继续跑完 |

所以「跳过」只保证 **Documents 恢复**，不保证 LPE 侧动作回滚：run 目录、`wallet.db`、
可能残留的服务，都按 `lpe\README.md` 的手动序列处理。用户点跳过之后看到
`%LOCALAPPDATA%\ClassroomBypass-LPE\<guid>` 里还有东西，不是 bug，是这条边界。

日志只写三个关键节点（心跳不加状态：200ms 一条 `abort=false` 只会淹掉日志，而 abort
之后的 3 秒超时根本不看心跳）：

1. `elevate.c` 写 abort：`lpe: user skipped, abort written, waiting 3000ms for trigger.ps1`
2. `trigger.ps1` 看到 abort：`documents: abort observed, exiting loop`
3. `elevate.c` 超时硬杀：`lpe: trigger.ps1 did not exit in 3000ms, terminating`

另外 `elevate.c` 写 abort 后顺手扫一次 `Get-Service -Name 'CVE49176_*'`（普通权限可以
枚举），有命中就 `ELog("note: service CVE49176_%lu_%lu may remain; see README for cleanup", ...)`，
不执行任何操作——只留一条指向检查方向的痕迹。

`3000ms` 用固定值，不跟 `TimeoutSec` 缩放：“用户主动放弃”不能等 45 秒。前提是
`do{}` 循环体里的单步操作要么 < 200ms，要么可以切成 200ms 片——否则 abort 写了也
只能等超时。

核对过 `trigger.ps1:316-325`，循环体是 `Start-Sleep -Milliseconds 200` + 一次
`Test-Path`，命中后 `Get-Content`/`Where-Object`/`Select-Object -Last 1` 读一个几十行的
`result.txt`——全部远小于 200ms，满足条件。

但有一个**已知的不可打断点**：`Await-WinRtOperation ($manager::RequestStoreAsync())`
（:311）在循环**之前**，abort 检查覆盖不到它。如果 WinRT 那次调用本身就挂住，abort
只能等 3000ms 超时后硬杀——这正好回到 `finally` 不跑、靠快照恢复的路径，不是新问题，
但要在实现时明确写进注释，免得以后有人以为 abort 能中断一切。

检查点必须放进 `do{}`（`:316`）——放在循环外等于 LPE 主逻辑跑完才检查，而主逻辑正常
要 10~30 秒、最长 45 秒。

## 5. 通道与权限

| 项 | 决定 |
|---|---|
| 路径 | `%LOCALAPPDATA%\ClassroomBypass-LPE\abort.<elevate-pid>` |
| 写 | 只有 `elevate.c`（普通权限，用户令牌） |
| 读 | 只有 `trigger.ps1`（high / SYSTEM），在 `do{}` 里每 200ms 一次 `Test-Path -LiteralPath` |
| 删 | 只有 `elevate.c`，在 `RunHidden` **之前**删（含上一次的残留） |
| 内容 | 一行 `abort <GetTickCount()>`，仅用于日志可读；语义是“文件存在” |

- 路径带 pid：上一次运行残留的 abort 不会误伤本次（本次 pid 不同）。
- 不需要 `bin\lpe\` 可写：安装目录可能只读，和快照同一个理由。
- 方向正确：medium 写的文件 high 能读（同一 SID）。反过来（high 写、medium 读）虽然
  通常也能读，但我们不需要这个方向。
- `trigger.ps1` 不删这个文件：只有写方删，才没有“两边都删/一方删了另一方还在等”的竞态。

## 6. Job Object

- `CreateJobObjectW` + `SetInformationJobObject(JobObjectExtendedLimitInformation,
  JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)`，在 `RunHidden` 成功后**立刻**
  `AssignProcessToJobObject(job, proc)`。
- **assign 与第一行脚本之间的窗口**：`CreateProcessA` 返回后到 assign 完成之间，
  powershell 可能已经跑了几条指令。这期间它 spawn 的子进程默认也进 job（父在 job 里），
  但“已经 spawn 完”的那一个会逃逸。`trigger.ps1` 开头只读文件、不 spawn，窗口的实际
  影响接近零；正确做法是 `CREATE_SUSPENDED` → assign → `ResumeThread`，把窗口降为零。
  `RunHidden` 只有一个调用点（`elevate.c:789`），加一个 `suspend` 参数不会影响别处，
  实现时直接这么做。
- **assign 失败的后果要说清**：`TerminateProcess(powershell)` 只杀宿主。
  `documents_path.exe set $root` 如果是被 assign 失败后硬杀留下的，它可能刚写完注册表
  就停在那里，而 `finally` 已经不会跑了。下次运行只能靠 `documents.orig` / 候选列表
  恢复——这就是“快照必须先落地”的直接理由。日志里要有一条
  `lpe: job assign failed err=%lu - hard kill will not cover child processes`。
- 句柄必须在硬杀之后、函数返回之前才关闭——`KILL_ON_JOB_CLOSE` 是“句柄关 = 杀”。
- `AssignProcessToJobObject` 失败（目标已在别的 job 里，`ERROR_ACCESS_DENIED` = 5，
  或嵌套 job 的 LimitFlags 不允许）：只写 `ELog`，不阻断流程。当前 `RunHidden` 用
  `CreateProcessA`（`elevate.c:265`），父进程不在 job 里，正常路径不会失败；
  **UAC 那条路径用 `ShellExecuteEx`（`elevate.c:573`），拿不到进程句柄，不建 job**。
- job 的作用域是 **powershell 及其默认子进程**。`conhost.exe` 是否在 job 里取决于
  启动标志（当前 `RunHidden` 用 `CREATE_NO_WINDOW`，现代 Windows 上通常随父），
  它不持有 Documents 重定向，所以**不作为残留判定依据**，也不进验收标准。
- 不需要 `CREATE_BREAKAWAY_FROM_JOB`：那是“父进程已在 job 里”才需要的技巧。

## 7. 四种时序

| 场景 | 结果 |
|---|---|
| 正常完成 | 脚本自己 `exit`，job 句柄关闭，powershell 已退出，无副作用 |
| 点跳过 | abort → finally 恢复 Documents → 删 abort → 3 秒内退出 |
| 点跳过且脚本卡住 | 3 秒后 `TerminateProcess`；Documents 停在 `$root`，快照仍在，下次运行修复 |
| 点跳过 + assign 失败 | 硬杀只杀 powershell；`documents_path.exe` 可能还在跑。Documents 可能停在 `$root`，靠下次运行的快照恢复；日志里有 assign 失败那一条 |
| `elevate.c` 被强杀 | job 句柄随进程关闭 → powershell 被杀；Documents 停在 `$root`，下次运行用快照修复 |

## 8. 验收标准

1. 点跳过：日志出现 `documents restored`，注册表两处 `Personal` 回到原值，abort 文件消失；
2. 跳过 + 脚本卡住（测试桩里 `Start-Sleep 10`）：3 秒后 powershell 消失，快照文件仍在；
3. 强杀 `elevate.c`（任务管理器）：powershell 跟着消失（`tasklist` 无残留）；
4. 已成功（握手行已出现）后点跳过：提权实例不受影响；
5. 三条日志齐全且顺序正确（写 abort → abort observed → 硬杀或正常退出）；
6. `conhost.exe` 不作为残留判定依据：验收只查中文名 powershell 是否存在；
7. `make all` 0 警告，全套测试通过（含 P1a 的 45 项）。

## 9. 已定（本轮答复）

| 问题 | 结论 |
|---|---|
| `3000ms` 是否缩放 | 固定值，不跟 `TimeoutSec` 缩放；前提是循环体单步 < 200ms 或可切片（当前满足） |
| 跳过时是否 `sc stop/delete` | 不做。`elevate.c` 是普通权限，删服务要么静默失败要么二次提权；只扫一次并 `ELog` 留痕 |
| README 是否列运行时文件 | 列，单独一节“运行时文件（不提交，不打包）”，已加 |
| 心跳是否带 abort 状态 | 不带，改用三条关键节点日志 |

## 10. 仍需确认

1. 残留服务的手工序列要不要做成 `build\cleanup_lpe.ps1`（管理员运行，先校验 `net session`）？
   现在是 README 里的文档，靠人照抄，容易漏掉“先 delete 再删目录”。
2. `documents_path.exe` 若在硬杀窗口里被留下，要不要在下次启动时检测并等待它退出？
   当前的答案是“不检测，靠快照恢复”，但日志里缺少“上次可能有个 set 正在跑”的痕迹。
