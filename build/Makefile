# ============================================================
#  Convenience wrapper: run the build from build\ as well.
#
#     cd build
#     mingw32-make            (or make.bat, which also finds the toolchain)
#
#  Everything is delegated to ..\make.bat: it discovers the toolchain and
#  exports it to PATH (gcc needs cc1 / as / ld / collect2 from its own bin
#  directory, which is why a bare "gcc -o" fails on a fresh machine).
#  No build logic here on purpose, so the target list cannot go stale.
# ============================================================

TARGETS := all bypass hook32 lpe lpebroker test wmodetest vdtest clean rebuild check

.PHONY: $(TARGETS)

$(TARGETS):
	@cmd /c "..\make.bat $@"
