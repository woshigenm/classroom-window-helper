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

检查点必须放进 `do{}`（`trigger.ps1` 现在的 216-225）——放在循环外等于 LPE 主逻辑跑完
才检查，而主逻辑正常要 10~30 秒、最长 45 秒。

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
- 句柄必须在硬杀之后、函数返回之前才关闭——`KILL_ON_JOB_CLOSE` 是“句柄关 = 杀”。
- `AssignProcessToJobObject` 失败（目标已在别的 job 里，`ERROR_ACCESS_DENIED` = 5）：
  只写 `ELog`，不阻断流程。当前 `RunHidden` 用 `CreateProcessA`
  （`elevate.c:265`），父进程不在 job 里，正常路径不会失败；**UAC 那条路径用
  `ShellExecuteEx`（`elevate.c:573`），拿不到进程句柄，不建 job**。
- 不需要 `CREATE_BREAKAWAY_FROM_JOB`：那是“父进程已在 job 里”才需要的技巧。

## 7. 四种时序

| 场景 | 结果 |
|---|---|
| 正常完成 | 脚本自己 `exit`，job 句柄关闭，powershell 已退出，无副作用 |
| 点跳过 | abort → finally 恢复 Documents → 删 abort → 3 秒内退出 |
| 点跳过且脚本卡住 | 3 秒后 `TerminateProcess`；Documents 停在 `$root`，快照仍在，下次运行修复 |
| `elevate.c` 被强杀 | job 句柄随进程关闭 → powershell 被杀；Documents 停在 `$root`，下次运行用快照修复 |

## 8. 验收标准

1. 点跳过：日志出现 `documents restored`，注册表两处 `Personal` 回到原值，abort 文件消失；
2. 跳过 + 脚本卡住（测试桩里 `Start-Sleep 10`）：3 秒后 powershell 消失，快照文件仍在；
3. 强杀 `elevate.c`（任务管理器）：powershell 跟着消失（`tasklist` 无残留）；
4. 已成功（握手行已出现）后点跳过：提权实例不受影响；
5. `make all` 0 警告，全套测试通过（含 P1a 的 45 项）。

## 9. 待确认

1. `3000ms` 是否合适：`finally` 里只有一次 `documents_path.exe set`（实测 < 100ms），
   剩下的余量是给 `do{}` 的轮询间隔。要不要按 `TimeoutSec` 缩放？
2. 跳过时是否也要 `sc stop/delete` 残留服务？倾向不做：脚本不一定有权限，而且会被
   误判成“每次跳过都动系统服务”。
3. `lpe\README.md` 的“文件”表要不要列出 `abort.<pid>` 与 `documents.orig`（英文名、
   都在 `%LOCALAPPDATA%`，不属于仓库目录）？
4. 这份设计落地时，是否顺便把 `lpe: still waiting` 的心跳行加上 `abort` 状态（一眼看出
   用户已经点了跳过）？
