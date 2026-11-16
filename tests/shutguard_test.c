/* ============================================================
 *  shutguard_test.c -- "ignore remote shutdown/restart" self test
 *
 *  The injected DLL hooks the places the classroom client uses to power the
 *  machine off (REA/Ghidra evidence in src/bypass_shut.inc):
 *      ExitWindowsEx                     -- 0x27 restart
 *      CreateProcessA/W / WinExec        -- 0x28 `shutdown -s -t 1`
 *      ShellExecuteA/W                   -- same, shell path
 *
 *  SAFETY: this test can never power the machine off.
 *    * It NEVER calls ExitWindowsEx / InitiateSystemShutdown.  Whether the
 *      restart hook is armed is read back from the DLL log instead.
 *    * Every shutdown-shaped command line points at a FAKE shutdown.exe:
 *      %TEMP%\shutguard_probe\shutdown.exe, a copy of cmd.exe.  Even with the
 *      guard disabled or broken, running it only prints a cmd error.
 *
 *  A/B plan:
 *    guard off : the fake shutdown.exe runs normally (a process is created)
 *                a normal child still starts and exits with 7
 *    guard on  : every shutdown spelling is swallowed, no process created
 *                normal children still work
 *                the log says the ExitWindowsEx hook is installed
 *
 *  Usage:
 *    shutguard_test.exe --expect on|off [--inject <bypass_hook32.dll>]
 *                       [<inject32.exe>]
 *  Exit code 0 = all checks passed.
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char  g_probeA[MAX_PATH];    /* ...\shutguard_probe\shutdown.exe */
static WCHAR g_probeW[MAX_PATH];
static char  g_logPath[MAX_PATH];

/* A copy of cmd.exe named shutdown.exe: the command line looks exactly like
   the one the classroom client uses, but running it does nothing at all. */
static int PrepareProbeExe(void)
{
    char tmp[MAX_PATH], sys[MAX_PATH], dir[MAX_PATH];
    char exe[MAX_PATH];
    char* p;

    g_probeA[0] = 0;
    g_probeW[0] = 0;
    if (!GetTempPathA(sizeof(tmp), tmp)) return 0;
    _snprintf(dir, sizeof(dir) - 1, "%sshutguard_probe", tmp);
    dir[sizeof(dir) - 1] = '\0';
    CreateDirectoryA(dir, NULL);
    _snprintf(g_probeA, sizeof(g_probeA) - 1, "%s\\shutdown.exe", dir);
    g_probeA[sizeof(g_probeA) - 1] = '\0';

    if (!GetSystemDirectoryA(sys, sizeof(sys))) return 0;
    if (strlen(sys) + 9 < sizeof(sys)) strcat(sys, "\\cmd.exe");
    else return 0;

    if (!CopyFileA(sys, g_probeA, FALSE) &&
        GetLastError() != ERROR_FILE_EXISTS)
        return 0;

    if (!MultiByteToWideChar(CP_ACP, 0, g_probeA, -1, g_probeW, MAX_PATH))
        return 0;

    /* log file next to this exe (bin\bypass_hook.log) */
    GetModuleFileNameA(NULL, exe, sizeof(exe));
    p = strrchr(exe, '\\');
    if (p) {
        *(p + 1) = '\0';
        _snprintf(g_logPath, sizeof(g_logPath) - 1, "%sbypass_hook.log", exe);
    } else {
        g_logPath[0] = '\0';
    }
    return 1;
}

/* Did the injected DLL log "<needle>" for this process? */
static int LogHas(const char* needle)
{
    char  line[1024];
    FILE* f;
    int   found = 0;

    if (!g_logPath[0]) return 0;
    f = fopen(g_logPath, "r");
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, needle)) { found = 1; break; }
    }
    fclose(f);
    return found;
}

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

/* Start a child and wait for it.  1 = started, 0 = not started.
   *pid != 0 only when a real process was created (i.e. NOT intercepted). */
static int SpawnW(const WCHAR* app, const WCHAR* cmdline, DWORD* pid)
{
    WCHAR   buf[2048];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    BOOL    ok;

    if (pid) *pid = 0;
    memset(&buf, 0, sizeof(buf));
    if (cmdline) {
        wcsncpy(buf, cmdline, 2047);
        buf[2047] = 0;
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    ok = CreateProcessW(app, cmdline ? buf : NULL, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        printf("    CreateProcessW failed err=%lu\n", (unsigned long)GetLastError());
        return 0;
    }
    if (pi.hProcess) {
        if (pid) *pid = pi.dwProcessId;
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    return 1;
}

static int SpawnA(const char* app, const char* cmdline, DWORD* pid)
{
    char    buf[2048];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    BOOL    ok;

    if (pid) *pid = 0;
    memset(&buf, 0, sizeof(buf));
    if (cmdline) {
        size_t n = strlen(cmdline);
        if (n > sizeof(buf) - 1) n = sizeof(buf) - 1;
        memcpy(buf, cmdline, n);
    }
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    ok = CreateProcessA(app, cmdline ? buf : NULL, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        printf("    CreateProcessA failed err=%lu\n", (unsigned long)GetLastError());
        return 0;
    }
    if (pi.hProcess) {
        if (pid) *pid = pi.dwProcessId;
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    return 1;
}

/* Child that proves normal process creation still works; exit code 7. */
static int PassthroughWorks(void)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD code = 0;
    char  cmd[128];
    BOOL  ok;

    _snprintf(cmd, sizeof(cmd) - 1, "cmd.exe /c exit 7");
    cmd[sizeof(cmd) - 1] = '\0';
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi);
    if (!ok) {
        printf("  %-46s FAIL (CreateProcess err=%lu)\n",
               "CreateProcessA normal child", (unsigned long)GetLastError());
        return 0;
    }
    if (!pi.hProcess) {                 /* our fake: blocked by mistake */
        printf("  %-46s FAIL (blocked a normal child)\n",
               "CreateProcessA normal child");
        CloseHandle(pi.hThread);
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("  %-46s %s (exit=%lu)\n", "CreateProcessA normal child",
           code == 7 ? "ok" : "FAIL", (unsigned long)code);
    return code == 7;
}

int main(int argc, char** argv)
{
    const char* dll      = NULL;
    const char* injector = NULL;
    int         expectOn = -1;
    int         pass     = 1;
    int         i;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--inject") && i + 1 < argc) dll = argv[++i];
        else if (!_stricmp(argv[i], "--expect") && i + 1 < argc) {
            expectOn = !_stricmp(argv[i + 1], "on");
            i++;
        } else if (!injector) injector = argv[i];
    }
    if (expectOn < 0) {
        printf("usage: shutguard_test.exe --expect on|off [--inject <dll>] [<inject32.exe>]\n");
        return 2;
    }
    if (!PrepareProbeExe()) {
        printf("FATAL: cannot stage the fake shutdown.exe (no test run)\n");
        return 4;
    }
    printf("probe exe: %s (copy of cmd.exe, runs nothing)\n", g_probeA);

    if (dll) {
        char buf[MAX_PATH];
        if (!injector) {
            char exe[MAX_PATH] = "";
            char* p;
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            p = strrchr(exe, '\\');
            if (p) *(p + 1) = '\0';
            _snprintf(buf, sizeof(buf) - 1, "%sinject32.exe", exe);
            injector = buf;
        }
        if (!RunInjector(injector, dll)) {
            printf("WARNING: injector failed, continuing as baseline\n");
            pass = 0;
        }
        Sleep(1800);                /* Worker + first ini read */
    }

    printf("mode: expect %s\n", expectOn ? "guard ON" : "guard off");

    if (!expectOn) {
        DWORD pid = 0;
        UINT  w;
        char  needle[160];
        WCHAR cmd[2048];

        /* With the guard off the fake shutdown.exe must be started normally.
           ★ MUST use the probe path: a bare "shutdown -s -t 1" resolves to
             the real System32\shutdown.exe and powers the machine off (this
             is exactly what happened once during development). */
        _snwprintf(cmd, 2047, L"\"%s\" -s -t 1", g_probeW);
        cmd[2047] = 0;
        if (!SpawnW(NULL, cmd, &pid) || pid == 0) {
            printf("  %-46s FAIL (fake shutdown.exe not started)\n",
                   "fake shutdown.exe runs (guard off)");
            pass = 0;
        } else {
            printf("  %-46s ok\n", "fake shutdown.exe runs (guard off)");
        }

        if (!PassthroughWorks()) pass = 0;

        w = WinExec("cmd.exe /c exit 0", SW_HIDE);
        printf("  %-46s %s (ret=%u)\n", "WinExec normal command",
               w > 31 ? "ok" : "FAIL", (unsigned)w);
        if (w <= 31) pass = 0;

        _snprintf(needle, sizeof(needle) - 1,
                  "[pid %lu] [Guard] remote shutdown/restart guard -> off",
                  (unsigned long)GetCurrentProcessId());
        if (LogHas(needle)) {
            printf("  %-46s ok\n", "log says guard is off");
        } else {
            printf("  %-46s FAIL (log line missing)\n", "log says guard is off");
            pass = 0;
        }

        printf("SUMMARY expect=off\n");
        printf("VERDICT %s\n", pass ? "PASS (guard off: nothing intercepted)"
                                    : "FAIL");
        return pass ? 0 : 1;
    }

    /* ---------------- guard ON ---------------- */
    {
        DWORD   pid = 0;
        int     blocked;
        UINT    w;
        INT_PTR h;
        WCHAR   cmd[2048];
        char    acmd[2048];
        char    needle[160];

        /* The first call is what proves the hook is in place.  It would run
           the harmless fake shutdown.exe if the hook were missing. */
        _snwprintf(cmd, 2047, L"\"%s\" -s -t 1", g_probeW);
        cmd[2047] = 0;
        blocked = SpawnW(NULL, cmd, &pid) && pid == 0;
        printf("  %-46s %s\n", "CreateProcessW(\"shutdown.exe\" -s -t 1)",
               blocked ? "ok" : "FAIL (fake exe ran)");
        if (!blocked) pass = 0;

        _snwprintf(cmd, 2047, L"\"%s\" /r /t 1", g_probeW);
        cmd[2047] = 0;
        blocked = SpawnW(NULL, cmd, &pid) && pid == 0;
        printf("  %-46s %s\n", "CreateProcessW(\"shutdown.exe\" /r)",
               blocked ? "ok" : "FAIL");
        if (!blocked) pass = 0;

        blocked = SpawnW(g_probeW, L"-s -t 1", &pid) && pid == 0;
        printf("  %-46s %s\n", "CreateProcessW(app, \"-s -t 1\")",
               blocked ? "ok" : "FAIL");
        if (!blocked) pass = 0;

        _snwprintf(cmd, 2047, L"cmd.exe /c \"%s\" -s -t 1", g_probeW);
        cmd[2047] = 0;
        blocked = SpawnW(NULL, cmd, &pid) && pid == 0;
        printf("  %-46s %s\n", "CreateProcessW(cmd /c shutdown ...)",
               blocked ? "ok" : "FAIL");
        if (!blocked) pass = 0;

        _snprintf(acmd, sizeof(acmd) - 1, "\"%s\" -s -t 1", g_probeA);
        acmd[sizeof(acmd) - 1] = 0;
        blocked = SpawnA(NULL, acmd, &pid) && pid == 0;
        printf("  %-46s %s\n", "CreateProcessA(shutdown.exe -s -t 1)",
               blocked ? "ok" : "FAIL");
        if (!blocked) pass = 0;

        w = WinExec(acmd, SW_HIDE);
        printf("  %-46s %s (ret=%u)\n", "WinExec(shutdown.exe -s -t 1)",
               w == 33 ? "ok" : "FAIL", (unsigned)w);
        if (w != 33) pass = 0;

        h = (INT_PTR)ShellExecuteW(NULL, L"open", g_probeW, L"-r -t 1", NULL, SW_HIDE);
        printf("  %-46s %s (ret=%ld)\n", "ShellExecuteW(shutdown.exe -r)",
               h == 33 ? "ok" : "FAIL", (long)h);
        if (h != 33) pass = 0;

        /* normal children must still work */
        if (!PassthroughWorks()) pass = 0;

        w = WinExec("cmd.exe /c exit 0", SW_HIDE);
        printf("  %-46s %s (ret=%u)\n", "WinExec normal command",
               w > 31 ? "ok" : "FAIL", (unsigned)w);
        if (w <= 31) pass = 0;

        /* Zero-risk proof for the 0x27 path: the DLL logs the result of its
           ExitWindowsEx entry hook.  Nothing calls the API here. */
        _snprintf(needle, sizeof(needle) - 1,
                  "[pid %lu] [Guard] hook ExitWindowsEx: installed",
                  (unsigned long)GetCurrentProcessId());
        if (LogHas(needle)) {
            printf("  %-46s ok\n", "ExitWindowsEx entry hook armed (log)");
        } else {
            printf("  %-46s FAIL (log line missing)\n",
                   "ExitWindowsEx entry hook armed (log)");
            pass = 0;
        }
    }

    printf("SUMMARY expect=on (no real shutdown.exe was ever started)\n");
    printf("VERDICT %s\n", pass ? "PASS (remote shutdown/restart swallowed)"
                                 : "FAIL");
    return pass ? 0 : 1;
}
