# ============================================================
#  Convenience wrapper - run GNU Make from build\ as well.
#  The real build rules are in ..\Makefile.
#
#     cd build
#     "C:\Program Files\RedPanda-Cpp\mingw64\bin\mingw32-make.exe"
# ============================================================

# 不用 $(MAKE)：cmd 传进来的 argv[0] 可能自带双引号，再包一层会变成 ""path""。
MAKE_EXE := C:/Program Files/RedPanda-Cpp/mingw64/bin/mingw32-make.exe

.PHONY: all bypass hook32 test clean rebuild check

all bypass hook32 test clean rebuild check:
	"$(MAKE_EXE)" -C .. $@
