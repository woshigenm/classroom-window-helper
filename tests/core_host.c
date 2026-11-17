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

static HWND g_hostWin;
static volatile LONG g_hammerRun;

static DWORD WINAPI HammerThread(LPVOID p)
{
    (void)p;
    while (InterlockedCompareExchange(&g_hammerRun, 0, 0)) {
        if (g_hostWin && IsWindow(g_hostWin))
            ShowWindow(g_hostWin, SW_SHOWNOACTIVATE);
        Sleep(50);
    }
    return 0;
}

static LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static const char* g_probeTimes;
static const char* g_probeLLTimes;

/* P3/reload test: at the given millisecond offsets install a WH_MOUSE_LL hook.
   While the Core is live it suppresses SetWindowsHookExW and returns
   (HHOOK)0x1; if the Loader's dispatchers are stuck in drain passthrough
   (the g_unloading / reload bug) the real hook is installed and a real handle
   comes back.  The proc only calls CallNextHookEx, so even a real install keeps
   the mouse working; it is unhooked immediately. */
static LRESULT CALLBACK ProbeLLProc(int code, WPARAM w, LPARAM l)
{
    return CallNextHookEx(NULL, code, w, l);
}

static DWORD WINAPI ProbeLLThread(LPVOID p)
{
    char  times[128];
    char* s;
    DWORD t0 = GetTickCount();

    (void)p;
    _snprintf(times, sizeof(times) - 1, "%s", g_probeLLTimes ? g_probeLLTimes : "");
    times[sizeof(times) - 1] = '\0';

    s = times;
    while (s && *s) {
        char* comma = strchr(s, ',');
        DWORD at = (DWORD)atoi(s);

        if (comma) *comma = '\0';
        while ((DWORD)(GetTickCount() - t0) < at) Sleep(20);
        {
            HHOOK hk = SetWindowsHookExW(WH_MOUSE_LL, ProbeLLProc, NULL, 0);

            printf("probe llhook t=%lu ret=%p\n", (unsigned long)at, (void*)hk);
            fflush(stdout);
            if (hk && hk != (HHOOK)(ULONG_PTR)0x1) UnhookWindowsHookEx(hk);
        }
        s = comma ? (comma + 1) : NULL;
    }
    return 0;
}

/* P3/reentry test: at the given millisecond offsets call WinExec with a
   shutdown-looking but *nonexistent* executable:
       C:\nonexistent\shutdown.exe -s -t 1
   While the shutdown guard is active the Core recognises the command (base
   name "shutdown" + a /s-style flag) and blocks it: WinExec returns 33 and
   "[Guard] blocked WinExec" is logged.  Once the Core is disabled (failed
   phase 2) the hook must pass through, so the real WinExec runs - the file
   does not exist, so it returns 0 and nothing happens.  No shutdown can ever
   be scheduled by this probe. */
static DWORD WINAPI ProbeThread(LPVOID p)
{
    static const char kProbe[] = "C:\\nonexistent\\shutdown.exe -s -t 1";
    char  times[128];
    char* s;
    DWORD t0 = GetTickCount();

    (void)p;
    _snprintf(times, sizeof(times) - 1, "%s", g_probeTimes ? g_probeTimes : "");
    times[sizeof(times) - 1] = '\0';

    s = times;
    while (s && *s) {
        char* comma = strchr(s, ',');
        DWORD at = (DWORD)atoi(s);

        if (comma) *comma = '\0';
        while ((DWORD)(GetTickCount() - t0) < at) Sleep(20);
        {
            UINT rc = WinExec(kProbe, SW_HIDE);
            printf("probe winexec t=%lu ret=%u\n", (unsigned long)at, (unsigned)rc);
            fflush(stdout);
        }
        s = comma ? (comma + 1) : NULL;
    }
    return 0;
}

int main(int argc, char** argv)
{
    const char* pidfile = NULL;
    const char* dll = NULL;
    const char* injector = NULL;
    int seconds = 30;
    int crashAfter = 0;
    int makeWindow = 0;
    int hammerShow = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--pidfile") && i + 1 < argc) pidfile = argv[++i];
        else if (!_stricmp(argv[i], "--inject") && i + 1 < argc) dll = argv[++i];
        else if (!_stricmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "--crash-after") && i + 1 < argc) crashAfter = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "--window")) makeWindow = 1;
        else if (!_stricmp(argv[i], "--hammer-show")) hammerShow = 1;
        else if (!_stricmp(argv[i], "--probe-winexec") && i + 1 < argc) g_probeTimes = argv[++i];
        else if (!_stricmp(argv[i], "--probe-llhook") && i + 1 < argc) g_probeLLTimes = argv[++i];
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

    if (makeWindow) {
        WNDCLASSA wc;
        int      sw = GetSystemMetrics(SM_CXSCREEN);
        int      sh = GetSystemMetrics(SM_CYSCREEN);
        HWND     w;

        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = HostProc;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.lpszClassName = "MultiRenderP2bTest";
        RegisterClassA(&wc);

        w = CreateWindowExA(WS_EX_NOACTIVATE, "MultiRenderP2bTest", "p2b subclass host",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            sw / 10, sh / 10, sw * 8 / 10, sh * 8 / 10,
                            NULL, NULL, wc.hInstance, NULL);
        if (w) ShowWindow(w, SW_SHOWNOACTIVATE);
        g_hostWin = w;
        printf("window=%p\n", (void*)w);
        fflush(stdout);

        if (hammerShow && w) {
            InterlockedExchange(&g_hammerRun, 1);
            CreateThread(NULL, 0, HammerThread, NULL, 0, NULL);
            printf("hammer=on\n");
            fflush(stdout);
        }
    }

    if (crashAfter > 0) {
        Sleep((DWORD)crashAfter);
        /* 未处理异常: 走 Loader 的 UEF 转发器 -> Core 的 CrashFilter 记录 */
        RaiseException(0xE0000001u, 0, 0, NULL);
    }

    if (g_probeTimes && g_probeTimes[0]) {
        HANDLE th = CreateThread(NULL, 0, ProbeThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    if (g_probeLLTimes && g_probeLLTimes[0]) {
        HANDLE th = CreateThread(NULL, 0, ProbeLLThread, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }

    if (seconds < 1) seconds = 1;
    if (seconds > 300) seconds = 300;
    Sleep((DWORD)seconds * 1000);
    return 0;
}
