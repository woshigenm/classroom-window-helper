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
#  Toolchain:
#     64-bit  C:\Program Files\RedPanda-Cpp\mingw64\bin\gcc.exe    (11.5.0)
#     32-bit  C:\mingw32\bin\i686-w64-mingw32-gcc.exe             (16.2.0)
# ============================================================

CC64  := C:/Program Files/RedPanda-Cpp/mingw64/bin/gcc.exe
CC32  := C:/mingw32/bin/i686-w64-mingw32-gcc.exe
MAKE  := C:/Program Files/RedPanda-Cpp/mingw64/bin/mingw32-make.exe
BIN   := bin

# gcc 需要能在 PATH 里找到 cc1/as/ld/collect2（.bat 脚本里也是先 set PATH）。
export PATH := C:/Program Files/RedPanda-Cpp/mingw64/bin;C:/mingw32/bin;$(PATH)

# 64-bit main program (GUI subsystem)
CFLAGS64 := -mwindows -O2 -D__USE_MINGW_ANSI_STDIO=1 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable
LIBS64   := -lgdi32 -lcomctl32 -ladvapi32 -lshell32 -luser32 -lshlwapi -lwinmm -liphlpapi

# 32-bit components (must be static for the injected DLL)
CFLAGS32 := -std=gnu11 -O2 -Wall
DLLFLAGS := -shared -lpsapi -Wl,--kill-at -static -static-libgcc

.PHONY: all bypass hook32 test clean rebuild check

# 默认：主程序 + DLL + 注入器（不含自测载体）
all: bypass hook32

bypass: $(BIN)/bypass.exe

hook32: $(BIN)/bypass_hook32.dll $(BIN)/inject32.exe

test: $(BIN)/hooktest32.exe

# ---- outputs -------------------------------------------------

$(BIN):
	@if not exist "$(BIN)" mkdir "$(BIN)"

$(BIN)/bypass.exe: src/bypass.c src/bypass_state.h | $(BIN)
	"$(CC64)" $(CFLAGS64) "src/bypass.c" -o "$@" $(LIBS64)

$(BIN)/bypass_hook32.dll: src/bypass_hook.c src/bypass_state.h | $(BIN)
	"$(CC32)" $(CFLAGS32) $(DLLFLAGS) "src/bypass_hook.c" -o "$@"

$(BIN)/inject32.exe: src/inject32.c | $(BIN)
	"$(CC32)" $(CFLAGS32) "src/inject32.c" -o "$@"

$(BIN)/hooktest32.exe: tests/hooktest32.c | $(BIN)
	"$(CC32)" $(CFLAGS32) -Wno-unused-variable "tests/hooktest32.c" -o "$@"

# ---- housekeeping --------------------------------------------

# ⚠ clean 会删除 bin\ 下的交付文件（以及自测载体）。
#    删完用 `make.bat all` 重新编译即可；不想删交付文件就不要跑 clean。
clean:
	-@del /q "$(BIN)\bypass.exe" "$(BIN)\bypass_hook32.dll" "$(BIN)\inject32.exe" 2>nul
	-@del /q "$(BIN)\hooktest32.exe" 2>nul
	@echo [clean] done

rebuild: clean all

# 确认两个编译器都能用（输出带版本号）
check:
	@"$(CC64)" --version | findstr /i "gcc.exe"
	@"$(CC32)" --version | findstr /i "gcc.exe"
	@echo [check] toolchains OK
