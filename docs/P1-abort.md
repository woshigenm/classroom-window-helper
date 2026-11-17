# P1b 设计：点「跳过」时的协作退出

状态：**已实现并真机验收**（abort 协作 + Job Object）。正常链路与「点跳过」都已在真机
各跑通一次（2026-11-17，见 §0 验证状态与 §8 验收进度）；剩余未覆盖的只有 §8 的 1b/2/3。

## 0. 实现记录（本轮）

| 位置 | 内容 |
|---|---|
| `lpe\trigger.ps1` | `do{}` 内新增 `# >>> abort-check` 块：发现 `abort.<AbortPid>` → 记 `documents: abort observed, exiting` → `return`（不是 `throw`，`return` 同样会跑 `finally`） |
| `src\elevate.c` | `SkipFilePath()` / `WriteAbort()` / `CreateKillOnCloseJob()` / `CloseJob()` / `LogLeftoverServices()`；`RunHidden()` 加 `job` 参数（`CREATE_SUSPENDED` → assign → 无条件 `ResumeThread`）；跳过时写 abort → 等 ≤3000ms → 超时才 `TerminateProcess` |
| `MaterializeScript()` / `BuildLpeCommand()` | 前置段（含 `$AbortPid=<launcher pid>`）与脚本一起写成 `%LOCALAPPDATA%\ClassroomBypass-LPE\trigger.<pid>.<tick>.ps1`（UTF-8 带 BOM），命令行只带路径：`-Command "Invoke-Expression (Get-Content -LiteralPath '<path>' -Raw)"`。`-EncodedCommand` 整条删除，理由见 §5.1。顺手修掉 `qTarget[900]` 对 `target[2048]` 的静默截断（截掉的是 `--elevate=uac`） |
| `tests\lpe_documents.ps1` | 新增 abort-check 三个用例（命中 / 无文件 / 无 pid + 残留 `abort.` 文件）+ `restore-fallback`，再加 IEX 指令形态 5 项（路径含 `'`、BOM、`return` 跑 `finally`、`exit N` 传播），共 72 项 |

`return` 在脚本作用域、`try/finally` 内是否真的跑 `finally` 不是靠读文档断言的：上面第三个
用例把真实形状（`try { do{} } finally {}` + `return`）抽出来跑，断言 `finally ran` 出现、
`after-block` 与 `tail` 都不出现。

### 验证状态：哪些是单元覆盖，哪些没跑过

| 项 | 覆盖方式 |
|---|---|
| abort 文件写入 / 轮询 / `return` 跑 `finally` | 单元（`tests\lpe_documents.ps1`，桩） |
| 3000ms 等待与超时 `TerminateProcess` | 代码审查 + 单元形状，未在真实链路上计时 |
| Job assign / `KILL_ON_JOB_CLOSE` / disarm | 本机一次性探针（armed close 杀、disarmed close 不杀、父在 job 内 assign 成功）+ 真机日志 `lpe: script is in the job (kill-on-close)`（2026-11-17） |
| 落盘脚本 + `-Command` IEX + `policy=Restricted` 下运行 + `finally` 恢复 Documents + 握手行 | **真机已验证**（2026-11-17，另一台机）：`policy=Restricted` 脚本照常执行，从落盘到 `elevated instance pid=4644 is up` 共 2.95s，`documents restored ... source=original`，`how=user-linked` |
| **点跳过 → 真 `elevate.c` 写文件 → 真 `trigger.ps1` 轮询 → 真 `finally` 恢复** | **真机已验证**（2026-11-17）：跳过写入后脚本 **47 ms** 退出（未动用 `TerminateProcess`），`documents restored ... source=original`，`note: no leftover CVE49176_* service`，abort 文件与落盘脚本正常清理 |

三处「只有真实链路才暴露」的耦合至此全部走完一遍：跨权限读文件、残留清理时序、3000ms
宽限（实际 47 ms，余量巨大）。但这次实测还暴露了一个当初推不出来的竞态，已修：

**skip 与成功的竞态。** 跳过落在了握手行之后 109 ms（`elevated instance pid=4712 is up` 在
t，skip 在 t+109 ms）。skip 分支一旦进入就只等脚本退出、不再扫握手行，所以 `ok` 保持 0 →
启动器报失败 → 多弹一次 UAC → 多起一个提权实例，靠单实例互斥体才没有变成双开。无害但
难看。修法：skip 收尾时做一次最终握手检查，链路已经成功就认账、不再弹第二个 UAC——
「跳过」跳过的是利用链，不是已经存在的结果。

因此：在 `§8` 跑过之前，**遇到「点了跳过但 Documents 没回来」，先看这条链路**，不要先怀疑
快照或候选列表。README 的运行时文件表也带同样的提示。

## 1. 先摆事实（读代码得出，不是推测）

```
bypass.exe（普通用户；elevate.c）
 └─ powershell.exe -Command "IEX (Get-Content <落盘的触发脚本>)"   ← RunHidden()
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
因为它覆盖的正是那个进程——`elevate.c` 退出后不会留下持有 Documents 重定向的孤儿
（assign 成功时；assign 失败的情形见 §6）。

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
| `trigger.ps1` | 看到 abort → 退出 `do{}` → `finally` **尝试**恢复 Documents → 退出 |
| `elevate.c` | 写 abort → 等 powershell 退出（≤3000ms）→ 超时硬杀 → 继续走 UAC / 返回 |
| 提权实例（broker 拉起） | 不受影响，继续跑完 |

### 「尝试恢复」不是「恢复」

不用「保证」这个词，因为链路上至少三处会失败：

| 环节 | 失败可能 |
|---|---|
| 轮询 | powershell 正卡在 `Await-WinRtOperation`（`:311`），abort 根本没被看到 → 3 秒后硬杀 |
| `finally` | `$original` 已不是目录 → 跳过（`restore skipped`）；`set` 返回非零 → `WARNING` |
| 写成功 | 写回的是**候选值**，不是用户原来的路径（例如原来是 `D:\MyDocs`，现在回到 `%USERPROFILE%\Documents`） |

第三条最危险：Documents 功能正常了，用户以为原路径回来了，其实没有。所以恢复日志要带
**来源标记**，三种来源分开写：

| source | 含义 | 能说「原路径回来了」吗 |
|---|---|---|
| `original` | 本次读到的就是真值，重定向前后写回同一个值 | 能 |
| `snapshot` | 用的是上一次运行留下的 `documents.orig` | 能（快照就是那次写的真值） |
| `fallback` | 快照不可用，用了 OneDrive / `%USERPROFILE%\Documents` 候选 | **不能**——只是“找了一个能用的” |

实现上是一个变量：`$source` 在 known-folder 块里跟着 `$original` 一起定（健康路径 =
`original`；用 `$recorded` 的两个分支 = `snapshot`；候选分支 = `fallback`），`finally`
只用它拼日志，例如

```
documents restored original=D:\MyDocs source=snapshot
documents restored original=C:\Users\x\Documents source=fallback
```

`source=fallback` 那一行就是「路径变了」的凭证：默认日志里看不到差异，只有这个标记能
事后区分「真的还原了」和「换了个能用的」。三者都不成立时不写入、留 `WARNING`。

同理，「跳过」不保证 LPE 侧动作回滚：run 目录、`wallet.db`、可能残留的服务，都按
`lpe\README.md` 的手动序列处理。用户点跳过之后看到
`%LOCALAPPDATA%\ClassroomBypass-LPE\<guid>` 里还有东西，不是 bug，是这条边界。

也不保证恢复到**用户最初配置的**路径：自定义重定向（`D:\MyDocs`、网络盘）如果原值已经
丢了（快照没写成，或者上一版把它覆盖了），P1 无法还原，只能人工
`bin\lpe\documents_path.exe set <真实路径>`。候选列表能救回的是「Documents 能用」，
不是「你的配置回来了」。

日志只写关键节点（心跳不加状态：200ms 一条 `abort=false` 只会淹掉日志，而 abort
之后的 3 秒超时根本不看心跳）：

1. `elevate.c` 写 abort：`lpe: user skipped, abort written, waiting up to 3000ms for trigger.ps1`
2. `trigger.ps1` 看到 abort：`documents: abort observed, exiting`
3. `elevate.c` 等到退出：`lpe: trigger.ps1 exited after %lu ms`
4. `elevate.c` 超时硬杀：`lpe: trigger.ps1 did not exit in 3000ms, terminating`

3 和 4 是互斥的，必须有一条。带实际时长是为了事后能看出「是不是经常卡满 3 秒」——
这决定要不要继续查 `trigger.ps1` 里的不可打断点。

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

### 5.1 脚本怎么交给 powershell：`-EncodedCommand` 已删

原来把 `trigger.ps1` 转成 UTF-16 再 Base64，塞进 `powershell.exe -EncodedCommand`。这条路
有个硬上限：命令行 32767 字符，而 UTF-16 + Base64 是 ~2.7 倍膨胀，脚本超过约 12 KB 就再
也起不来。`trigger.ps1` 长到 19 KB 之后，**每次** LPE 都在这一行失败并退回 UAC——单元
测试全绿，真实链路 100% 走不到。日志里长这样：

```
lpe: encoded command does not fit in 32767 bytes (-1 needed) - is the payload script too big?
lpe failed - falling back to uac/manual
```

现在：前置段 + 脚本写成 `%LOCALAPPDATA%\ClassroomBypass-LPE\trigger.<pid>.<tick>.ps1`
（UTF-8 **带 BOM**），命令行只带路径：

```
powershell.exe -NoLogo -NoProfile -NonInteractive -Command "Invoke-Expression (Get-Content -LiteralPath '<path>' -Raw)"
```

四个不能改错的点：

| 点 | 为什么 |
|---|---|
| 用 `-Command`，不用 `-File` | 执行策略（含 GPO「禁止运行脚本」，它连 `-ExecutionPolicy Bypass` 都能盖掉）只约束 `-File` 启动的脚本文件；`-Command` 的文本不查策略。`Get-Content` 只是读数据，同样不查。 |
| 必须带 BOM | Windows PowerShell 5.1 对无 BOM 文件按 ANSI 代码页解码；前置段里可能有非 ASCII 的安装路径，无 BOM 会被解成乱码。`trigger.ps1` 自身是纯 ASCII。 |
| 路径放**单引号**里，`'` 写成 `''` | 外层是双引号参数，路径里不会有 `"`；`PsEscape()` 只处理 `'`，而且**装不下就返回失败**，不截断（截断会指向另一个文件）。 |
| 临时文件由启动器删 | 脚本在子进程的第一条语句就被 `Get-Content` 读进去，握手/退出/60s 上限都在那之后；硬杀会留下一个，`DeleteStaleScripts()` 下次同 pid 清掉，`build\cleanup_lpe.ps1` 也能列出/清理。 |

`--elevate-selftest` 跟着改了：它现在把脚本落到 `%TEMP%`，并把**完整命令行**写进日志，
不再产出 `.b64`。要判断「这次 LPE 为什么退回 UAC」，先看这两行，再把命令行贴进控制台。

## 6. Job Object

- `CreateJobObjectW` + `SetInformationJobObject(JobObjectExtendedLimitInformation,
  JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE)`，在 `RunHidden` 成功后**立刻**
  `AssignProcessToJobObject(job, proc)`。
- **assign 与第一行脚本之间的窗口**：`CreateProcessA` 返回后到 assign 完成之间，
  powershell 可能已经跑了几条指令。这期间它 spawn 的子进程默认也进 job（父在 job 里），
  但“已经 spawn 完”的那一个会逃逸。`trigger.ps1` 开头只读文件、不 spawn，窗口的实际
  影响接近零——但「现在不 spawn」是**未来改动的隐含约束**，不该靠它。已定：
  `CREATE_SUSPENDED` → assign → `ResumeThread`，把窗口降为零。
  `RunHidden` 只有一个调用点（`elevate.c:789`），加一个参数不影响别处。

- **suspend 的三条铁律**（写错比不 suspend 更糟，按这个顺序实现）：

  | 步骤 | 必须 | 写错的后果 |
  |---|---|---|
  | `CreateProcessA(..., CREATE_SUSPENDED \| CREATE_NO_WINDOW, ...)` | 失败时**不** resume（没有线程存在） | `ResumeThread` 一个不存在的进程 |
  | `AssignProcessToJobObject` | 成功和失败**都** `ELog`（失败带 `err=%lu`） | 失败无声，日志里看起来“有 job 保护” |
  | `ResumeThread(pi.hThread)` | **无条件执行，且在 assign 之后** | assign 失败时忘了 resume → 进程永久挂起，用户视角是“UAC 过了但什么都没发生” |

  「assign 失败也要 resume」是最容易写出来的 bug：直觉上“保护没建立就不该让它跑”，
  但那会让进程僵死在挂起态。正确的降级是 **没有 job 保护地正常跑**，由快照兜底。

- `ResumeThread` 在 `RunHidden` 内部、返回之前执行，所以调用点（`:789`）永远看不到一个
  挂起态的进程；`RunHidden` 之后任何“进程已启动”的假设（取输出、等退出）都成立。

- **assign 失败的后果要说清**：`TerminateProcess(powershell)` 只杀宿主。
  `documents_path.exe set $root` 如果是被 assign 失败后硬杀留下的，它可能刚写完注册表
  就停在那里，而 `finally` 已经不会跑了。下次运行只能靠 `documents.orig` / 候选列表
  恢复——这就是“快照必须先落地”的直接理由。日志里要有一条
  `lpe: job assign failed err=%lu - hard kill will not cover child processes`。
- 句柄必须在硬杀之后、函数返回之前才关闭——`KILL_ON_JOB_CLOSE` 是“句柄关 = 杀”。

- **成功路径必须先撤掉 `KILL_ON_JOB_CLOSE`，再关句柄**（否则这是本次设计里最容易自伤的一处）。
  循环在“看到握手行”时就 `break`（`elevate.c:820`），而那一刻 trigger.ps1 **通常还在跑**：
  它可能刚写完 `command.txt`、还没走到 `finally`。如果这时按“函数返回前关闭 job 句柄”处理，
  `KILL_ON_JOB_CLOSE` 会把正在跑 `finally` 的 powershell 杀掉——Documents 停在 `$root`，
  而且**每一次成功都会发生**，比原来的“点跳过才残留”严重得多。

  规则只有一条：**任何正常返回都先撤掉 kill-on-close，再关句柄**；显式杀由
  `TerminateProcess` 负责。不做“按路径决定撤不撤”——那种表格迟早会漏一行，而漏掉成功
  路径的代价是“每次成功都把 Documents 留在 `$root`”。

  换句话说：job 的语义是「**父进程意外消失时**连带杀掉脚本」，不是「父进程函数返回时杀掉
  脚本」。这两件事在成功路径上不是一回事。

  这条规则在本机用一次性探针验证过（不是推理）：`CreateProcess(CREATE_SUSPENDED)` →
  `AssignProcessToJobObject` → `ResumeThread` → 撤掉 limit → `CloseHandle`，子进程存活；
  不撤、直接 `CloseHandle`，子进程被杀；`AssignProcessToJobObject` 在**父进程本身也在 job
  里**的情况下依然返回成功（Windows 8+ 的嵌套 job），所以 `ERROR_ACCESS_DENIED` 那条分支
  只作为兜底存在，不是常规路径。
- `AssignProcessToJobObject` 失败（目标已在别的 job 里且嵌套不被允许，或 job 的
  LimitFlags 不允许）：只写 `ELog`，不阻断流程。`RunHidden` 用 `CreateProcessA`
  （`elevate.c:265`），本机探针显示父进程本身在 job 里时 assign 仍返回成功
  （Windows 8+ 的嵌套 job），所以失败分支是兜底而不是常规路径；
  **UAC 那条路径用 `ShellExecuteEx`（`elevate.c:573`），拿不到进程句柄，不建 job**。
- job 的作用域是 **powershell 及其默认子进程**。`conhost.exe` 是否在 job 里取决于
  启动标志（当前 `RunHidden` 用 `CREATE_NO_WINDOW`，现代 Windows 上通常随父），
  它不持有 Documents 重定向，所以**不作为残留判定依据**，也不进验收标准。
- 不需要 `CREATE_BREAKAWAY_FROM_JOB`：那是“父进程已在 job 里”才需要的技巧。

## 7. 五种时序

| 场景 | 结果 |
|---|---|
| 正常完成 | 脚本自己 `exit` → `finally` 恢复 Documents → elevate.c 撤掉 kill-on-close 再关句柄，不留孤儿也不误杀 |
| 点跳过 | abort → finally 尝试恢复 Documents → 删 abort → 3 秒内退出 |
| 点跳过且脚本卡住 | 3 秒后 `TerminateProcess`；Documents 停在 `$root`，快照仍在，下次运行修复 |
| 点跳过 + assign 失败 | 硬杀只杀 powershell；`documents_path.exe` 可能还在跑。Documents 可能停在 `$root`，靠下次运行的快照恢复；日志里有 assign 失败那一条 |
| `elevate.c` 被强杀 | job 句柄随进程关闭 → powershell 被杀；Documents 停在 `$root`，下次运行用快照修复 |

## 8. 验收标准

1. 点跳过：日志出现 `documents restored ... source=<original|snapshot|fallback>`，**source 与实际来源一致**（用快照恢复时不能写 `source=original`），注册表两处 `Personal` 回到该值，abort 文件消失；
2. 跳过 + 脚本卡住（测试桩里 `Start-Sleep 10`）：3 秒后 powershell 消失，快照文件仍在，日志有 `did not exit in 3000 ms, terminating`；
3. 强杀 `elevate.c`（任务管理器）：powershell 跟着消失（`tasklist` 无残留）；
4. 已成功（握手行已出现）后点跳过：提权实例不受影响；
5. 关键节点日志齐全且顺序正确（写 abort → abort observed → `exited after N ms` 或 `terminating`，两者恰有一条）；
6. `conhost.exe` 不作为残留判定依据：验收只查 powershell 是否存在；
7. **成功路径不误杀**：握手行出现后（不点跳过）等 10 秒，日志里没有“脚本被杀”的痕迹，Documents 已由 `finally` 恢复（不是停在 `$root`）。这条专门验证 §6 的“撤掉 kill-on-close”；
8. `make all` 0 警告，全套测试通过（`lpe documents state` 72 项）。

**验收进度**（2026-11-17，DESKTOP-UCUPIMQ 真机）：1 的 `source=original` 形态、4（以竞态
形态真实发生：skip 落在握手后 109 ms，提权实例 4712 不受影响；修复后更是不再弹第二个
UAC）、5（写 abort → `exited after 47 ms`，恰一条）、7 的“不误杀”半边已覆盖。
未覆盖：1 的 `snapshot`/`fallback` 形态、2（脚本卡住被硬杀）、3（强杀 elevate.c）——
三条都只有单元/探针形状，再遇真实环境时优先补这三条。

## 9. 已定（本轮答复）

| 问题 | 结论 |
|---|---|
| `3000ms` 是否缩放 | 固定值，不跟 `TimeoutSec` 缩放；前提是循环体单步 < 200ms 或可切片（当前满足）。**实际等待时长要写进日志**：`exited after %lu ms` / `did not exit in 3000ms`，否则永远看不出是不是经常打满 |
| 跳过时是否 `sc stop/delete` | 不做。`elevate.c` 是普通权限，删服务要么静默失败要么二次提权；只扫一次并 `ELog` 留痕 |
| README 是否列运行时文件 | 列，单独一节“运行时文件（不提交，不打包）”，已加 |
| 心跳是否带 abort 状态 | 不带，改用关键节点日志（写 abort / abort observed / exited 或 terminating） |

关于 `Get-Service -Name 'CVE49176_*'`：**扫到即真残留**，不留“可能正在删除中”的模糊话。
`DeleteService` 成功后服务只是被标记删除，而枚举由新开的 SC Manager 句柄发起，看不到
已标记删除的项。所以能看到就说明进程死在 `CreateServiceW` 与 `DeleteService` 之间。

### 一个小概率但真实的撞名窗口（不修，记账）

`CVE49176_<pid>_<GetTickCount()>` 严格说不是「不可能撞」：`GetTickCount` 49.7 天回绕，
pid 也会复用。同一次「回绕后 tick 恰好相等 + pid 恰好复用 + 那个残留服务还在」才会撞。

按你的建议本来该加三行（`ERROR_SERVICE_EXISTS` → `OpenService` + `DeleteService` →
再 `CreateServiceW` 一次），但**不能加**：`lpe\src\*.c` 与上游逐字节相同是硬约束
（只允许来源注释头和 `#ifndef`），改它等于把冻结的源码解冻，而
`build\check_payload.ps1` 只比对「源码 vs 记录的哈希」，抓不到这种偏离。

查了源码，撞名的后果是**已定义且良性**的：`CreateServiceW` 的失败是被检查的
（`wallet_callback_shell.c:126-130`），不会把 NULL 句柄传给 `StartServiceW` /
`DeleteService`；它把 `ERROR_SERVICE_EXISTS`（1073）返回给 WalletService，
`trigger.ps1` 记下 `shell failed`，`elevate.c` 退回 UAC。也就是「这一次 LPE 没成，
退回原流程」，不是未定义行为。代价只是那一次不能自动提权——不值得为此解冻上游源码。

## 10. 结论（本轮）

1. **`build\cleanup_lpe.ps1`：做，但默认只列不删。**
   这套工具里最危险的自动化就是清理脚本——它的输入是「系统当前状态」，而那个状态可能
   包含**正在运行的正常实例**。无条件 `sc delete CVE49176_*` + `rmdir` 会让用户在一次
   正常运行途中删掉服务与 DLL，LPE 失败且日志里看不出原因。

   | 行为 | 参数 |
   |---|---|
   | 列出 `CVE49176_*` 服务、`%LOCALAPPDATA%\ClassroomBypass-LPE\` 下的 runid 目录、`documents.orig`、`abort.*` | 无参数（默认） |
   | 清理指定项 | `-Service <name>` / `-RunId <id>` |
   | 全清 | `-All -Force`，且要求 `net session` 通过 + 二次确认 |

    存活判断不能只看 pid：服务名里的 pid 是 **WalletService（SYSTEM，几乎总活着）** 的
    pid，不是提权实例的 pid（`wallet_callback_shell.c:114` 的 `GetCurrentProcessId()`）。
    按 pid 判活会把每一条残留都标成「运行中」，永远清理不掉。实际用的是两条证据：
    服务 `State -ne 'Stopped'`，以及**从 `PathName` 反解出 runid 目录**后有进程的
    `ExecutablePath`/`CommandLine` 落在该目录里。`abort.*` / `trigger.*.ps1` 才按 pid
    判活，且要求进程名是 `bypass.exe`（pid 被别的映像复用 → 按残留处理）。

2. **遗留 `documents_path.exe` 检测：不做。** 对象选错了：它的生命周期是「set 完就退」，
   被逮到的概率极低，而且它不持有状态——真正持有重定向的是跑 `trigger.ps1` 的
   powershell。退一步说，要检测也该检测「命令行含 `trigger.ps1` 且父进程已死的
   powershell」，但它是用 `-EncodedCommand` 起的（`elevate.c:265`），命令行里只有
   Base64，没有明文可匹配。A+ 的 stale 分支 + 快照已经在「下次运行」这个时点兜住了，
   再加一条启动时扫描就是第二条路径，两者判断不一致时更难查。留给 P3 和
   `ScanHookLog*` 一起做（那时才有进程枚举的基础设施）。

3. **P3 的起点判定：用偏移，不用 banner。** 「日志框第一行是不是 banner」这个问题不用
   再回答——它是 UI 实现细节，改版就会变，拿它当增量读的锚点等于把重构绑在展示层上。
   改成：点「检查当前状态」时记下文件长度 `L`（或行数 `N`），增量读从 `L` 开始；
   若 `L` 大于当前长度（文件被清空/轮转），直接从头读。这样日志格式怎么改都不影响。
