# ============================================================
#  bypass_tool - GNU Make build
#
#  Usage (make is not on PATH by default):
#     make.bat                  -> all (bypass + hook32)
#     make.bat bypass
#     make.bat hook32
#     make.bat test
#     make.bat clean / rebuild / check
#
#  or add the toolchain to PATH first:
#     set PATH=C:\Program Files\RedPanda-Cpp\mingw64\bin;%PATH%
#     mingw32-make
#
#  All compiled outputs go to bin\ (gitignored).
#  The .bat scripts in build\ remain supported and are the reference.
#  Toolchain (as found on the target machine):
#     32-bit  C:\Program Files (x86)\RedPanda-Cpp\mingw32\bin (i686-w64-mingw32-gcc 15.3.0)
#     64-bit  optional; when absent the main program is built 32-bit as well
#             (the delivered bin\bypass.exe is x86 anyway).
#  Override explicitly when you have a 64-bit gcc:
#     make.bat CC64="C:/path/to/x86_64-w64-mingw32-gcc.exe"
# ============================================================

MINGW32 := C:/Program Files (x86)/RedPanda-Cpp/mingw32/bin

CC32  := $(MINGW32)/i686-w64-mingw32-gcc.exe
CC64  ?= $(CC32)
MAKE  := $(MINGW32)/mingw32-make.exe
BIN   := bin

# WalletService auto-elevation payload (see lpe\README.txt):
#   bin\lpe\ = trigger.ps1 + vendored PoC binaries + shell_broker.exe (ours)
LPEOUT := $(BIN)/lpe
LPEPAY := lpe/payload

# gcc ???????????? PATH ????????? cc1/as/ld/collect2???.bat ?????????????????? set PATH??????
export PATH := $(MINGW32);$(PATH)

# 64-bit main program (GUI subsystem)
CFLAGS64 := -mwindows -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
LIBS64   := -lgdi32 -lcomctl32 -ladvapi32 -lshell32 -luser32 -lshlwapi -lwinmm -liphlpapi

# 32-bit components (must be static for the injected DLL)
CFLAGS32 := -std=gnu11 -O2 -Wall
DLLFLAGS := -shared -Wl,--kill-at -static -static-libgcc
DLLLIBS  := -lpsapi -lole32 -ladvapi32

.PHONY: all bypass hook32 lpe lpebroker test wmodetest vdtest clean rebuild check

# ?????????????????? + DLL + ?????????????????????????????????
all: bypass hook32 lpe

bypass: $(BIN)/bypass.exe

hook32: $(BIN)/bypass_hook32.dll $(BIN)/inject32.exe

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

# ---- outputs -------------------------------------------------

$(BIN):
	@if not exist "$(BIN)" mkdir "$(BIN)"

$(BIN)/bypass.exe: src/bypass.c src/elevate.c src/elevate.h src/bypass_state.h | $(BIN)
	"$(CC64)" $(CFLAGS64) "src/bypass.c" "src/elevate.c" -o "$@" $(LIBS64)

$(BIN)/bypass_hook32.dll: src/bypass_hook.c src/bypass_vd.inc src/bypass_state.h | $(BIN)
	"$(CC32)" $(CFLAGS32) $(DLLFLAGS) "src/bypass_hook.c" -o "$@" $(DLLLIBS)

$(BIN)/inject32.exe: src/inject32.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "src/inject32.c" -o "$@"

$(BIN)/hooktest32.exe: tests/hooktest32.c | $(BIN)
	"$(CC32)" $(CFLAGS32) -Wno-unused-variable "tests/hooktest32.c" -o "$@"

$(BIN)/wmode_test.exe: tests/wmode_test.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/wmode_test.c" -o "$@"

$(BIN)/vd_pin_test.exe: tests/vd_pin_test.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "tests/vd_pin_test.c" -o "$@" -lole32 -luuid -ladvapi32

# 32-bit on purpose: a demand-start service may be WoW64, and the toolchain is
# the same one used for the injected DLL.
$(LPEOUT)/shell_broker.exe: src/lpe_broker.c | $(BIN)
	@if not exist "$(LPEOUT)" mkdir "$(LPEOUT)"
	"$(CC32)" $(CFLAGS32) "src/lpe_broker.c" -o "$@" -luserenv -lwtsapi32 -ladvapi32 -lshell32

# ---- housekeeping --------------------------------------------

# ??? clean ????????? bin\ ?????????????????????????????????????????????
#    ????????? `make.bat all` ?????????????????????????????????????????????????????? clean???
clean:
	-@del /q "$(BIN)\bypass.exe" "$(BIN)\bypass_hook32.dll" "$(BIN)\inject32.exe" 2>nul
	-@del /q "$(BIN)\hooktest32.exe" 2>nul
	-@del /q "$(BIN)\wmode_test.exe" 2>nul
	-@del /q "$(BIN)\vd_pin_test.exe" 2>nul
	-@rmdir /s /q "$(BIN)\lpe" 2>nul
	@echo [clean] done

rebuild: clean all

# ??????????????????????????????????????????????????????
check:
	@"$(CC64)" --version | findstr /i "gcc.exe"
	@"$(CC32)" --version | findstr /i "gcc.exe"
	@echo [check] toolchains OK
