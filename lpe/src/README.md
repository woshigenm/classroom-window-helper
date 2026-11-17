# lpe\src\ —— WalletService PoC 源码

这里的三个 `.c` 来自 CVE-2026-49176 的 WalletService PoC，原样收录，目的是**可以直接改**，
不用再回到 PoC 仓库。上游用 MSVC（`cl /MT`，x64）构建；本仓库用 mingw-w64 也能构建，
见下面的"构建"。

| 文件 | 产物 | 作用 |
|---|---|---|
| `documents_path.c` | `payload\documents_path.exe` (x64) | `get` / `set <dir>`：读写 `FOLDERID_Documents`（`SHGetKnownFolderPath` / `SHSetKnownFolderPath`），不经过 Explorer |
| `wallet_ese_seed.c` | `payload\wallet_ese_seed.exe` (x64) | 造 `<dir>\Wallet\wallet.db`：`Cards` 表 + `JET_bitColumnUserDefinedDefault` 持久化回调，指向 `DLL!Export` |
| `wallet_callback_shell.c` | `payload\wallet_callback_shell.dll` (x64) | 被 WalletService 以 LocalSystem 加载；确认自己真的是 SYSTEM，然后 `CreateService` + `StartService` 拉起 `shell_broker.exe`，并把 `event=...` 追加到同目录 `result.txt` |

**没有收 `shell_broker.c`。** 上游那个 broker 直接拉起 `cmd.exe`；本工具需要拉起
`bin\bypass.exe`，并且要用交互会话里用户的令牌（而不是 SYSTEM），所以
`..\..\src\lpe_broker.c` 是按同样骨架重写的版本，它才是 `bin\lpe\shell_broker.exe` 的来源。

## 构建

```bat
make.bat lpepayload    :: 用 mingw 重新编译上面三个 -> lpe\payload\
make.bat lpe           :: 再把 lpe\payload\ 和 trigger.ps1 铺到 bin\lpe\
```

实际命令（与 `Makefile` 里的 `lpepayload` 一致）：

```bat
gcc -municode -O2 -Wall -o lpe\payload\documents_path.exe       lpe\src\documents_path.c       -lshell32 -lole32 -luuid
gcc -shared   -O2 -Wall -o lpe\payload\wallet_callback_shell.dll lpe\src\wallet_callback_shell.c -ladvapi32
gcc           -O2 -Wall -o lpe\payload\wallet_ese_seed.exe       lpe\src\wallet_ese_seed.c       -lesent
```

- `-municode`：`documents_path.c` 用的是 `wmain`（宽字符 `argv`）。
- `-luuid`：`FOLDERID_Documents` 的 GUID 常量在 `libuuid.a` 里。
- `-lesent`：ESE / `Jet*` API。mingw-w64 自带 `libesent.a`。
- `-shared`：回调 DLL。x64 没有 stdcall 名字修饰，导出名就是 `WalletCallback`（`objdump -p` 可验证）。

`lpepayload` 是**显式目标**，不进 `make.bat all`：`lpe\payload\` 里现在放的是上游 MSVC
构建的二进制，重编会改变 SHA256（`lpe\payload\README.txt` 里记的是原值），所以这件事
应该是一次有意识的动作，而不是每次构建都顺手做。

为了避免"改了源码、却静默拷了旧二进制"，`make.bat lpe` 会先跑
`build\check_payload.ps1`：把 `lpe\src\*.c` 的内容哈希和 `lpe\payload\SOURCES.sha256`
比对，不一致就拒绝拷贝并提示先跑 `lpepayload`。`lpepayload` 编译成功后会重写这个文件。
用内容哈希而不是时间戳，因为 `git clone` 会把所有文件的 mtime 写成同一时刻。

## 对同一份源码做过/没做过的验证

用 mingw 构建的这三个文件，已在本机验证：

- `documents_path.exe get` 能正确打印当前 Documents；
- `wallet_callback_shell.dll` 导出 `WalletCallback`（x64，无修饰）；
- `wallet_ese_seed.exe` 真的能建出 ESE 库（`seeded=... table=Cards column=256`），
  不涉及提权、不碰 Documents。

**没有**验证过的是整条链路（DLL 被 WalletService 加载、服务拉起 broker、拿到管理员进程）。
那需要在能跑的机器上点一次，见 `..\README.md` 的"手动自测"。

## 已知问题

- `documents_path.exe get` 把宽字符路径经 `wprintf` 输出到管道/文件时走的是 CRT 的
  `"C"` locale，非 ASCII 字符会被丢掉（`C:\Users\张三\Documents` → `C:\Users\\Documents`）。
  上游 MSVC 构建很可能同样如此，待实测确认。`trigger.ps1` 目前用它的输出来记原路径，
  这与"恢复 Documents"直接相关——修复方案见仓库提交记录，不要依赖这个 `get` 的输出。
- `wallet_ese_seed.exe` 的 `argv` 是 ANSI（`main` + `A` 系列 API）。在中文系统上
  （ANSI = CP936）中文用户名没问题；真正超出 ANSI 字符集的路径会传坏，那一步会失败
  并 `exit 3`，不会造成损坏。
