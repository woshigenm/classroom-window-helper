/* ============================================================
 *  core_host.c -- long-lived host for the P2a reload tests.
 *
 *  Self-injects bypass_hook32.dll, writes its PID to --pidfile and sleeps.
 *  The test script drives reloads over \\.\pipe\bypass_core_ctl_<pid>.
 *
 *  Usage: core_host.exe --pidfile <path> --inject <dll> [--seconds N]
 *  32-bit, C, ASCII only.
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int RunInjector(const char* injector, const char* dll)
{
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1;

    _snprintf(cmd, sizeof(cmd) - 1, "\"%s\" --pid %lu \"%s\"",
              injector, (unsigned long)GetCurrentProcessId(), dll);
    cmd[sizeof(cmd) - 1] = '\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        printf("inject: CreateProcess failed err=%lu\n", (unsigned long)GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("inject: exit=%lu\n", (unsigned long)rc);
    return rc == 0;
}

int main(int argc, char** argv)
{
    const char* pidfile = NULL;
    const char* dll = NULL;
    const char* injector = NULL;
    int seconds = 30;
    int crashAfter = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--pidfile") && i + 1 < argc) pidfile = argv[++i];
        else if (!_stricmp(argv[i], "--inject") && i + 1 < argc) dll = argv[++i];
        else if (!_stricmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "--crash-after") && i + 1 < argc) crashAfter = atoi(argv[++i]);
        else if (!injector) injector = argv[i];
    }

    if (dll) {
        char buf[MAX_PATH], exe[MAX_PATH] = "";
        char* p;

        if (!injector) {
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            p = strrchr(exe, '\\');
            if (p) *(p + 1) = '\0';
            _snprintf(buf, sizeof(buf) - 1, "%sinject32.exe", exe);
            buf[sizeof(buf) - 1] = '\0';
            injector = buf;
        }
        if (!RunInjector(injector, dll))
            printf("WARNING: injector failed\n");
    }

    if (pidfile && pidfile[0]) {
        FILE* f = fopen(pidfile, "w");
        if (f) {
            fprintf(f, "%lu\n", (unsigned long)GetCurrentProcessId());
            fclose(f);
        }
    }

    printf("pid=%lu ready\n", (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    if (crashAfter > 0) {
        Sleep((DWORD)crashAfter);
        /* 未处理异常: 走 Loader 的 UEF 转发器 -> Core 的 CrashFilter 记录 */
        RaiseException(0xE0000001u, 0, 0, NULL);
    }

    if (seconds < 1) seconds = 1;
    if (seconds > 300) seconds = 300;
    Sleep((DWORD)seconds * 1000);
    return 0;
}
