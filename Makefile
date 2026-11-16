# ============================================================
#  classroom-window-helper - GNU Make build
#
#  Usage (make is not on PATH by default):
#     make.bat                  -> all (bypass + hook32 + lpe)
#     make.bat bypass / hook32 / lpe
#     make.bat test / wmodetest / vdtest
#     make.bat clean / rebuild / check
#
#  or add the toolchain to PATH first:
#     set PATH=C:\mingw32\bin;%PATH%
#     mingw32-make
#
#  All compiled outputs go to bin\ (gitignored).
#  The .bat scripts in build\ remain supported and are the reference.
#  Toolchain: 32-bit i686-w64-mingw32-gcc.  It is auto-detected below (same
#  order as make.bat); override with MINGW32=... or make.bat CC32=...
#  64-bit gcc is optional: without it the main program is built 32-bit too
#  (the delivered bin\bypass.exe is x86 anyway).
# ============================================================

# 32-bit toolchain: first existing candidate wins (keep in sync with make.bat).
MINGW32 := $(shell if exist "C:\Program Files (x86)\RedPanda-Cpp\mingw32\bin\i686-w64-mingw32-gcc.exe" echo C:/Program Files (x86)/RedPanda-Cpp/mingw32/bin)
ifeq ($(strip $(MINGW32)),)
MINGW32 := $(shell if exist "C:\Program Files\RedPanda-Cpp\mingw32\bin\i686-w64-mingw32-gcc.exe" echo C:/Program Files/RedPanda-Cpp/mingw32/bin)
endif
ifeq ($(strip $(MINGW32)),)
MINGW32 := $(shell if exist "C:\mingw32\bin\i686-w64-mingw32-gcc.exe" echo C:/mingw32/bin)
endif
ifeq ($(strip $(MINGW32)),)
$(error 32-bit MinGW not found - run make.bat, or pass MINGW32=<bin dir>)
endif

CC32  := $(MINGW32)/i686-w64-mingw32-gcc.exe
CC64  ?= $(CC32)
BIN   := bin

# WalletService auto-elevation payload (see lpe\README.md):
#   bin\lpe\ = trigger.ps1 + vendored PoC binaries + shell_broker.exe (ours)
LPEOUT := $(BIN)/lpe
LPEPAY := lpe/payload

# gcc 还要在 PATH 里找到自己的 cc1 / as / ld / collect2，所以工具链目录必须
# 导出成 PATH —— 直接用 mingw32-make 跑（不经 make.bat）时尤其重要。
export PATH := $(MINGW32);$(PATH)

# 64-bit main program (GUI subsystem)
CFLAGS64 := -mwindows -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
LIBS64   := -lgdi32 -lcomctl32 -ladvapi32 -lshell32 -luser32 -lshlwapi -lwinmm -liphlpapi

# 32-bit components (must be static for the injected DLL)
CFLAGS32 := -std=gnu11 -O2 -Wall
DLLFLAGS := -shared -Wl,--kill-at -static -static-libgcc
DLLLIBS  := -lpsapi -lole32 -ladvapi32 -lgdi32

.PHONY: all bypass hook32 lpe lpebroker test wmodetest vdtest hangwintest screentest shuttest fakecores testcores corehost clean rebuild check

# 主程序 + DLL + 注入器 + 自动提权载荷（自测程序按需单独编）
all: bypass hook32 lpe

bypass: $(BIN)/bypass.exe

hook32: $(BIN)/bypass_hook32.dll $(BIN)/bypass_hook_core.dll $(BIN)/inject32.exe

# WalletService auto-elevation payload: bin\lpe\ (self-contained folder)
lpe: lpebroker
	@if not exist "$(LPEOUT)" mkdir "$(LPEOUT)"
	@copy /y "$(LPEPAY)\wallet_ese_seed.exe" "$(LPEOUT)\" >nul
	@copy /y "$(LPEPAY)\wallet_callback_shell.dll" "$(LPEOUT)\" >nul
	@copy /y "$(LPEPAY)\documents_path.exe" "$(LPEOUT)\" >nul
	@copy /y "$(LPEPAY)\README.txt" "$(LPEOUT)\" >nul
	@copy /y "lpe\trigger.ps1" "$(LPEOUT)\" >nul
	@echo [lpe] payload staged in bin\lpe

lpebroker: $(LPEOUT)/shell_broker.exe

test: $(BIN)/hooktest32.exe

# 控制端「窗口模式广播」小窗测试载体 (见 tests/wmode_test.c)
wmodetest: $(BIN)/wmode_test.exe

# virtual desktop pin regression (tests/vd_pin_test.c)
vdtest: $(BIN)/vd_pin_test.exe

# "对端不泵消息"的受害者进程, 配合 tests\foreign_fix_test.ps1 验证跨进程兜底
# 不会卡死 enforcer (tests/hangwin_test.c)
hangwintest: $(BIN)/hangwin_test.exe

# 屏幕监控守卫自测载体 (tests/screenguard_test.c)
screentest: $(BIN)/screenguard_test.exe

# 远程重启/关机守卫自测载体 (tests/shutguard_test.c)
shuttest: $(BIN)/shutguard_test.exe

# 故意损坏的 Core（Loader 失败路径：返回 0 / ABI 不符 / hook 表不全）
fakecores: $(BIN)/fake_core_return0.dll $(BIN)/fake_core_abi.dll $(BIN)/fake_core_hooks.dll $(BIN)/fake_core_leak.dll $(BIN)/fake_core_good.dll

# P2b-3: real Core built with a deliberate unload fault
testcores: $(BIN)/core_stuck_hook.dll $(BIN)/core_stuck_worker.dll $(BIN)/core_subclass_fail.dll $(BIN)/core_evil.dll

# 长驻宿主: 自注入 Loader 后睡眠, 供 P2a 重载脚本通过命名管道驱动
corehost: $(BIN)/core_host.exe

# UI harness: 真实主窗口 (含 bypass.c), 供按钮/托盘路径的脚本驱动
uitest: $(BIN)/ui_preview.exe

# ---- outputs -------------------------------------------------

$(BIN):
	@if not exist "$(BIN)" mkdir "$(BIN)"

$(BIN)/bypass.exe: src/bypass.c src/elevate.c src/elevate.h src/bypass_state.h src/bypass_hash.h | $(BIN)
	"$(CC64)" $(CFLAGS64) "src/bypass.c" "src/elevate.c" -o "$@" $(LIBS64)

$(BIN)/ui_preview.exe: tests/ui_preview.c src/bypass.c src/bypass_hash.h src/bypass_state.h src/elevate.c | $(BIN)
	"$(CC64)" $(CFLAGS64) "tests/ui_preview.c" "src/elevate.c" -o "$@" $(LIBS64)

$(BIN)/bypass_hook32.dll: src/loader.c src/bypass_abi.h src/bypass_hash.h | $(BIN)
	"$(CC32)" $(CFLAGS32) $(DLLFLAGS) "src/loader.c" -o "$@" -lpsapi -luser32

$(BIN)/bypass_hook_core.dll: src/bypass_hook.c src/bypass_vd.inc src/bypass_screen.inc src/bypass_shut.inc src/bypass_abi.h src/bypass_state.h | $(BIN)
	"$(CC32)" $(CFLAGS32) $(DLLFLAGS) "src/bypass_hook.c" -o "$@" $(DLLLIBS)

$(BIN)/inject32.exe: src/inject32.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "src/inject32.c" -o "$@"

$(BIN)/hooktest32.exe: tests/hooktest32.c | $(BIN)
	"$(CC32)" $(CFLAGS32) -Wno-unused-variable "tests/hooktest32.c" -o "$@"

$(BIN)/wmode_test.exe: tests/wmode_test.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/wmode_test.c" -o "$@"

$(BIN)/hangwin_test.exe: tests/hangwin_test.c | $(BIN)
	"$(CC64)" $(CFLAGS32) "tests/hangwin_test.c" -o "$@"

$(BIN)/screenguard_test.exe: tests/screenguard_test.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/screenguard_test.c" -o "$@" -lgdi32 -luser32

$(BIN)/shutguard_test.exe: tests/shutguard_test.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/shutguard_test.c" -o "$@" -lshell32 -luser32

$(BIN)/fake_core_return0.dll: tests/fake_core.c src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DFAKE_MODE=0 $(DLLFLAGS) "tests/fake_core.c" -o "$@"

$(BIN)/fake_core_abi.dll: tests/fake_core.c src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DFAKE_MODE=1 $(DLLFLAGS) "tests/fake_core.c" -o "$@"

$(BIN)/fake_core_hooks.dll: tests/fake_core.c src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DFAKE_MODE=2 $(DLLFLAGS) "tests/fake_core.c" -o "$@"

$(BIN)/fake_core_leak.dll: tests/fake_core.c src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DFAKE_MODE=3 $(DLLFLAGS) "tests/fake_core.c" -o "$@"

$(BIN)/fake_core_good.dll: tests/fake_core.c src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DFAKE_MODE=4 $(DLLFLAGS) "tests/fake_core.c" -o "$@"

$(BIN)/core_host.exe: tests/core_host.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/core_host.c" -o "$@"

$(BIN)/core_stuck_hook.dll: src/bypass_hook.c src/bypass_vd.inc src/bypass_screen.inc src/bypass_shut.inc src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DBYPASS_STUCK_HOOK $(DLLFLAGS) "src/bypass_hook.c" -o "$@" $(DLLLIBS)

$(BIN)/core_stuck_worker.dll: src/bypass_hook.c src/bypass_vd.inc src/bypass_screen.inc src/bypass_shut.inc src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DBYPASS_STUCK_WORKER $(DLLFLAGS) "src/bypass_hook.c" -o "$@" $(DLLLIBS)

$(BIN)/core_subclass_fail.dll: src/bypass_hook.c src/bypass_vd.inc src/bypass_screen.inc src/bypass_shut.inc src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DBYPASS_SUBCLASS_FAIL $(DLLFLAGS) "src/bypass_hook.c" -o "$@" $(DLLLIBS)

$(BIN)/core_evil.dll: src/bypass_hook.c src/bypass_vd.inc src/bypass_screen.inc src/bypass_shut.inc src/bypass_abi.h | $(BIN)
	"$(CC32)" $(CFLAGS32) -DBYPASS_EVIL_CORE $(DLLFLAGS) "src/bypass_hook.c" -o "$@" $(DLLLIBS)

$(BIN)/vd_pin_test.exe: tests/vd_pin_test.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/vd_pin_test.c" -o "$@" -lole32 -luuid -ladvapi32

# 32-bit on purpose: a demand-start service may be WoW64, and the toolchain is
# the same one used for the injected DLL.
$(LPEOUT)/shell_broker.exe: src/lpe_broker.c | $(BIN)
	@if not exist "$(LPEOUT)" mkdir "$(LPEOUT)"
	"$(CC32)" $(CFLAGS32) "src/lpe_broker.c" -o "$@" -luserenv -lwtsapi32 -ladvapi32 -lshell32

# ---- housekeeping --------------------------------------------

# 只删 bin\ 里的编译产物（三个交付文件也在内），不碰运行期配置和日志。
#    想恢复就重新 `make.bat all`；正在运行的 bypass.exe 会锁住文件，先退出。
clean:
	-@del /q "$(BIN)\bypass.exe" "$(BIN)\bypass_hook32.dll" "$(BIN)\bypass_hook_core.dll" "$(BIN)\inject32.exe" 2>nul
	-@del /q "$(BIN)\hooktest32.exe" 2>nul
	-@del /q "$(BIN)\wmode_test.exe" 2>nul
	-@del /q "$(BIN)\vd_pin_test.exe" 2>nul
	-@del /q "$(BIN)\screenguard_test.exe" 2>nul
	-@del /q "$(BIN)\shutguard_test.exe" 2>nul
	-@del /q "$(BIN)\fake_core_return0.dll" "$(BIN)\fake_core_abi.dll" "$(BIN)\fake_core_hooks.dll" "$(BIN)\fake_core_leak.dll" "$(BIN)\fake_core_good.dll" "$(BIN)\core_host.exe" "$(BIN)\core_stuck_hook.dll" "$(BIN)\core_stuck_worker.dll" "$(BIN)\core_subclass_fail.dll" "$(BIN)\core_evil.dll" "$(BIN)\ui_preview.exe" 2>nul
	-@rmdir /s /q "$(BIN)\lpe" 2>nul
	@echo [clean] done

rebuild: clean all

# 打印两个工具链版本，顺便确认上面的路径探测是否命中。
check:
	@"$(CC64)" --version | findstr /i "gcc.exe"
	@"$(CC32)" --version | findstr /i "gcc.exe"
	@echo [check] toolchains OK
