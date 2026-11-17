/*
 * bypass_shut.inc -- swallow the classroom console's remote shutdown/restart.
 *
 * Evidence (REA / Ghidra, Student.exe sha256 fcb7ab..473f):
 *   0x27 restart : FUN_00413560 -> FUN_00413080 -> FUN_00403a20
 *                  AdjustTokenPrivileges(SeShutdownPrivilege) + ExitWindowsEx(6,0)
 *   0x28 shutdown: FUN_00413560 -> FUN_00412d80 (CShutDownDlg) -> FUN_00412d40
 *                  -> systemoper.dll!SYS_RunCommand("shutdown -s -t 1")
 *
 * systemoper.dll is resolved at run time (GetProcAddress("SYS_RunCommand"),
 * NOT an import), so IAT patching alone would miss it the first time it is
 * used: the DLL is loaded, called and only later patched.  That is why the
 * interception is done on the function entries themselves (inline hooks),
 * which apply to every module in this process regardless of its IAT:
 *
 *   ExitWindowsEx          -> restart (0x27)
 *   CreateProcessA/W       -> SYS_RunCommand -> shutdown.exe
 *   WinExec                -> same, older code paths
 *   ShellExecuteA/W        -> same, shell paths
 *   InitiateSystemShutdown*-> not used by the client, cheap to cover
 *
 * Behaviour: when enabled, a matching call is not forwarded; it returns
 * success (TRUE / 33 / fake HINSTANCE) so the console does not see an error
 * and does not retry.  Everything else is passed through untouched.
 *
 * Config: bin\bypass_shut.ini next to the DLL, re-read twice a second.
 *   enabled=0   off (default)
 *   enabled=1   ignore remote shutdown/restart
 *
 * Pure ASCII on purpose (this file is #included by bypass_hook.c).
 */

#define SH_INI_NAME     "bypass_shut.ini"
#define SH_LOG_MAX      3       /* first 3 blocked calls in full ... */
#define SH_LOG_EVERY    25      /* ... then one heartbeat every 25th */
#define SH_WBUF         1024

/* typedefs live in bypass_abi.h (shared with the Loader) */
static PFN_ExitWindowsEx              real_ExitWindowsEx;
static PFN_WinExec                    real_WinExec;
static PFN_ShellExecuteA              real_ShellExecuteA;
static PFN_ShellExecuteW              real_ShellExecuteW;
static PFN_InitiateSystemShutdownA    real_InitiateSystemShutdownA;
static PFN_InitiateSystemShutdownW    real_InitiateSystemShutdownW;
static PFN_InitiateSystemShutdownExA  real_InitiateSystemShutdownExA;
static PFN_InitiateSystemShutdownExW  real_InitiateSystemShutdownExW;

static volatile LONG g_shutEnabled;    /* 0 = off (default), 1 = on */
static volatile LONG g_shutHits;       /* blocked calls */
static DWORD         g_shutCfgTick;
static int           g_shutInitLogged;

/* Defined later in bypass_hook.c; the entry hooks are installed from here so
   that a late-loaded systemoper.dll cannot slip through an unpatched IAT. */
static BOOL WINAPI Hook_CreateProcessA(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
                                       LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCSTR, LPSTARTUPINFOA,
                                       LPPROCESS_INFORMATION);
static BOOL WINAPI Hook_CreateProcessW(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                       LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                       LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                       LPPROCESS_INFORMATION);

/* ---------------- config: bin\bypass_shut.ini ------------------------------- */

static void SH_ReadConfig(void)
{
    char   path[MAX_PATH];
    FILE*  f;
    char   line[128];
    LONG   enabled = 0;

    if (g_shutCfgTick && GetTickCount() - g_shutCfgTick < 500) return;
    g_shutCfgTick = GetTickCount();

    if (!g_baseDir[0]) return;
    _snprintf(path, sizeof(path) - 1, "%s" SH_INI_NAME, g_baseDir);
    path[sizeof(path) - 1] = '\0';

    f = fopen(path, "r");
    if (!f) {
        /* no file = feature off (the main program creates it when enabled) */
        InterlockedExchange(&g_shutEnabled, 0);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char* eq;
        char* k;
        char* v;

        k = line;
        while (*k == ' ' || *k == '\t') k++;
        if (!*k || *k == '#' || *k == ';') continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = '\0';
        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        {
            size_t n = strlen(v);
            while (n && (v[n - 1] == '\r' || v[n - 1] == '\n' ||
                         v[n - 1] == ' '  || v[n - 1] == '\t'))
                v[--n] = '\0';
        }

        if (!_stricmp(k, "enabled")) enabled = (atoi(v) != 0) ? 1 : 0;
    }
    fclose(f);

    InterlockedExchange(&g_shutEnabled, enabled);
}

/* ---------------- command-line classification ------------------------------ */

static WCHAR SH_LowerW(WCHAR c)
{
    return (c >= L'A' && c <= L'Z') ? (WCHAR)(c + 32) : c;
}

/* Lowercase the executable name of a command line / app path, drop ".exe". */
static void SH_BaseNameW(const WCHAR* path, WCHAR* out, int n)
{
    const WCHAR* s;
    const WCHAR* e;
    const WCHAR* b;
    const WCHAR* q;
    int          i = 0;

    out[0] = '\0';
    if (!path) return;
    s = path;
    while (*s == L' ' || *s == L'\t') s++;
    if (*s == L'"') s++;
    e = s;
    while (*e && *e != L'"' && *e != L' ' && *e != L'\t') e++;
    b = s;
    for (q = s; q < e; q++)
        if (*q == L'\\' || *q == L'/') b = q + 1;
    for (q = b; q < e && i < n - 5; q++) out[i++] = SH_LowerW(*q);
    out[i] = '\0';
    if (i > 4 && out[i - 4] == L'.' && out[i - 3] == L'e' &&
        out[i - 2] == L'x' && out[i - 1] == L'e')
        out[i - 4] = '\0';
}

/* Any /s /r /p /g (or -s -r -p -g) token? Those all mean "power off". */
static int SH_FlagInArgsW(const WCHAR* s)
{
    while (s && *s) {
        while (*s == L' ' || *s == L'\t') s++;
        if (!*s) break;

        if (*s == L'"') {                 /* skip a quoted argument */
            s++;
            while (*s && *s != L'"') s++;
            if (*s) s++;
            continue;
        }
        if ((*s == L'/' || *s == L'-') && s[1]) {
            WCHAR c = SH_LowerW(s[1]);
            if (c == L's' || c == L'r' || c == L'p' || c == L'g')
                return 1;
        }
        while (*s && *s != L' ' && *s != L'\t') s++;
    }
    return 0;
}

/* Case-insensitive substring search. */
static int SH_HasW(const WCHAR* hay, const WCHAR* needle)
{
    int i, j;

    if (!hay || !needle || !*needle) return 0;
    for (i = 0; hay[i]; i++) {
        for (j = 0; needle[j]; j++)
            if (!hay[i + j] || SH_LowerW(hay[i + j]) != SH_LowerW(needle[j]))
                break;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* app = lpApplicationName (may be NULL), cmd = lpCommandLine (may be NULL). */
static int SH_IsShutdownCmdW(const WCHAR* app, const WCHAR* cmd)
{
    WCHAR        base[64];
    const WCHAR* args;

    SH_BaseNameW((app && app[0]) ? app : cmd, base, (int)(sizeof(base) / sizeof(base[0])));
    if (!base[0]) return 0;

    if (!wcscmp(base, L"shutdown")) {
        /* when both are given the switches live in lpCommandLine */
        args = (app && app[0]) ? (cmd ? cmd : L"") : cmd;
        return SH_FlagInArgsW(args);
    }

    /* cmd /c shutdown -s -t 1 , powershell -Command "Stop-Computer" ... */
    if (!wcscmp(base, L"cmd") || !wcscmp(base, L"powershell") ||
        !wcscmp(base, L"pwsh")) {
        if (!cmd) return 0;
        if (SH_HasW(cmd, L"stop-computer") || SH_HasW(cmd, L"restart-computer"))
            return 1;
        if (SH_HasW(cmd, L"shutdown"))
            return SH_FlagInArgsW(cmd);
    }
    return 0;
}

static int SH_IsShutdownCmdA(const char* app, const char* cmd)
{
    WCHAR wa[SH_WBUF], wc[SH_WBUF * 2];

    wa[0] = L'\0';
    wc[0] = L'\0';
    if (app && app[0])
        MultiByteToWideChar(CP_ACP, 0, app, -1, wa, SH_WBUF);
    if (cmd && cmd[0])
        MultiByteToWideChar(CP_ACP, 0, cmd, -1, wc, SH_WBUF * 2);
    if (!app && !cmd) return 0;
    return SH_IsShutdownCmdW(wa, wc);
}

/* ---------------- logging + decision --------------------------------------- */

/* The console retries every ~20 s, so an identical line per hit is noise.
   Log the first few in full, then only a heartbeat every SH_LOG_EVERY. */
static void SH_LogBlock(const char* what)
{
    LONG n = InterlockedIncrement(&g_shutHits);

    if (n <= SH_LOG_MAX) {
        HookLog("[Guard] blocked %s (total=%ld)", what, (long)n);
    } else if ((n % SH_LOG_EVERY) == 0) {
        HookLog("[Guard] blocked %ld remote power commands (last: %s)",
                (long)n, what);
    }
}

static int SH_On(void)
{
    return (g_shutEnabled == 1) ? 1 : 0;
}

/* 0x28 会先弹 CShutDownDlg（窗口类 L"ShutDownDlg"，Student.exe FUN_0040d560
   返回的就是这个字符串）再执行 shutdown。CreateProcess 已经被吞掉，但倒计时
   窗还是会弹出来，用户看到的是"系统将在 N 秒后关闭"，然后什么都没发生。
   这里在窗口创建后立即隐藏。

   故意**不**投递 WM_CLOSE：DuiLib 的 WindowImplBase::OnClose 往往直接
   PostQuitMessage(0)，在非模态场景会把 Student.exe 的整个消息循环结束掉。
   隐藏就够了 —— 倒计时走完照样会调 SYS_RunCommand，而那条路已经被
   CreateProcess 钩子拦下，不会再关机。 */
static volatile LONG g_shutDlgLogged;

static int SH_SuppressDialog(HWND hwnd, LPCWSTR cls)
{
    wchar_t name[64];
    char    a[64];

    if (!hwnd || !SH_On()) return 0;

    /* class atom (MAKEINTATOM) 不是字符串指针，先取回真实类名 */
    if (!cls || (ULONG_PTR)cls < 0x10000) {
        if (!GetClassNameW(hwnd, name, 64)) return 0;
        cls = name;
    }
    if (!SH_HasW(cls, L"ShutDown")) return 0;

    ShowWindow(hwnd, SW_HIDE);
    if (InterlockedIncrement(&g_shutDlgLogged) == 1) {
        a[0] = '\0';
        WideCharToMultiByte(CP_ACP, 0, cls, -1, a, sizeof(a) - 1, NULL, NULL);
        a[sizeof(a) - 1] = '\0';
        HookLog("[Guard] shutdown dialog suppressed (class=%s)", a);
    }
    return 1;
}

static int SH_BlockA(const char* app, const char* cmd, const char* api)
{
    char what[420];

    if (!SH_On() || !SH_IsShutdownCmdA(app, cmd)) return 0;
    _snprintf(what, sizeof(what) - 1, "%s (%s)", api,
              (cmd && cmd[0]) ? cmd : (app && app[0] ? app : "?"));
    what[sizeof(what) - 1] = '\0';
    SH_LogBlock(what);
    return 1;
}

static int SH_BlockW(const WCHAR* app, const WCHAR* cmd, const char* api)
{
    char a[384], what[420];
    const WCHAR* src;

    if (!SH_On() || !SH_IsShutdownCmdW(app, cmd)) return 0;

    src = (cmd && cmd[0]) ? cmd : app;
    a[0] = '\0';
    if (src) {
        WideCharToMultiByte(CP_ACP, 0, src, -1, a, sizeof(a) - 1, NULL, NULL);
        a[sizeof(a) - 1] = '\0';
    }
    _snprintf(what, sizeof(what) - 1, "%s (%s)", api, a[0] ? a : "?");
    what[sizeof(what) - 1] = '\0';
    SH_LogBlock(what);
    return 1;
}

/* ---------------- hooks ---------------------------------------------------- */

static BOOL WINAPI SH_Hook_ExitWindowsEx(UINT flags, DWORD reason)
{
    if (CORE_DISABLED())
        return real_ExitWindowsEx ? real_ExitWindowsEx(flags, reason) : FALSE;

    if (SH_On()) {
        char what[96];
        const char* kind = "logoff";

        /* EWX_REBOOT=0x2 (0x27), EWX_SHUTDOWN=0x1 (0x28), EWX_POWEROFF=0x8 */
        if (flags & EWX_REBOOT)        kind = "restart";
        else if (flags & EWX_SHUTDOWN) kind = "shutdown";
        else if (flags & EWX_POWEROFF) kind = "poweroff";

        _snprintf(what, sizeof(what) - 1,
                  "ExitWindowsEx flags=0x%lx (%s)",
                  (unsigned long)flags, kind);
        what[sizeof(what) - 1] = '\0';
        SH_LogBlock(what);
        SetLastError(0);
        return TRUE;                     /* pretend it worked: no retry */
    }
    return real_ExitWindowsEx ? real_ExitWindowsEx(flags, reason) : FALSE;
}

static UINT WINAPI SH_Hook_WinExec(LPCSTR cmd, UINT show)
{
    if (CORE_DISABLED()) return real_WinExec ? real_WinExec(cmd, show) : 0;
    if (SH_BlockA(NULL, cmd, "WinExec")) return 33;   /* > 31 = success */
    return real_WinExec ? real_WinExec(cmd, show) : 0;
}

static HINSTANCE WINAPI SH_Hook_ShellExecuteA(HWND owner, LPCSTR op, LPCSTR file,
                                              LPCSTR args, LPCSTR dir, INT show)
{
    if (CORE_DISABLED())
        return real_ShellExecuteA ? real_ShellExecuteA(owner, op, file, args, dir, show)
                                  : (HINSTANCE)0;
    if (SH_BlockA(file, args, "ShellExecuteA"))
        return (HINSTANCE)33;
    return real_ShellExecuteA ? real_ShellExecuteA(owner, op, file, args, dir, show)
                              : (HINSTANCE)0;
}

static HINSTANCE WINAPI SH_Hook_ShellExecuteW(HWND owner, LPCWSTR op, LPCWSTR file,
                                              LPCWSTR args, LPCWSTR dir, INT show)
{
    if (CORE_DISABLED())
        return real_ShellExecuteW ? real_ShellExecuteW(owner, op, file, args, dir, show)
                                  : (HINSTANCE)0;
    if (SH_BlockW(file, args, "ShellExecuteW"))
        return (HINSTANCE)33;
    return real_ShellExecuteW ? real_ShellExecuteW(owner, op, file, args, dir, show)
                              : (HINSTANCE)0;
}

static BOOL WINAPI SH_Hook_InitiateSystemShutdownA(LPSTR machine, LPSTR msg, DWORD timeout,
                                                   BOOL force, BOOL reboot)
{
    if (SH_On()) {
        SH_LogBlock("InitiateSystemShutdownA");
        return TRUE;
    }
    return real_InitiateSystemShutdownA
           ? real_InitiateSystemShutdownA(machine, msg, timeout, force, reboot) : FALSE;
}

static BOOL WINAPI SH_Hook_InitiateSystemShutdownW(LPWSTR machine, LPWSTR msg, DWORD timeout,
                                                   BOOL force, BOOL reboot)
{
    if (SH_On()) {
        SH_LogBlock("InitiateSystemShutdownW");
        return TRUE;
    }
    return real_InitiateSystemShutdownW
           ? real_InitiateSystemShutdownW(machine, msg, timeout, force, reboot) : FALSE;
}

static BOOL WINAPI SH_Hook_InitiateSystemShutdownExA(LPSTR machine, LPSTR msg, DWORD timeout,
                                                     BOOL force, BOOL reboot, DWORD reason)
{
    if (SH_On()) {
        SH_LogBlock("InitiateSystemShutdownExA");
        return TRUE;
    }
    return real_InitiateSystemShutdownExA
           ? real_InitiateSystemShutdownExA(machine, msg, timeout, force, reboot, reason)
           : FALSE;
}

static BOOL WINAPI SH_Hook_InitiateSystemShutdownExW(LPWSTR machine, LPWSTR msg, DWORD timeout,
                                                     BOOL force, BOOL reboot, DWORD reason)
{
    if (SH_On()) {
        SH_LogBlock("InitiateSystemShutdownExW");
        return TRUE;
    }
    return real_InitiateSystemShutdownExW
           ? real_InitiateSystemShutdownExW(machine, msg, timeout, force, reboot, reason)
           : FALSE;
}

/* ---------------- setup + heartbeat ---------------------------------------- */

static void SH_Init(void)
{
    SH_ReadConfig();

    /* The inline entries (WinExec, CreateProcessA/W, ExitWindowsEx,
       ShellExecuteA/W) are installed by the Loader; the real pointers are the
       trampolines from HrHost.real.  Only the InitiateSystemShutdown* fallbacks
       (which are not inline-hooked) need wiring here. */
    if (g_host) {
        real_WinExec             = (PFN_WinExec)g_host->real.WinExec;
        real_ExitWindowsEx       = (PFN_ExitWindowsEx)g_host->real.ExitWindowsEx;
        real_ShellExecuteA       = (PFN_ShellExecuteA)g_host->real.ShellExecuteA;
        real_ShellExecuteW       = (PFN_ShellExecuteW)g_host->real.ShellExecuteW;
        real_InitiateSystemShutdownA   = (PFN_InitiateSystemShutdownA)g_host->real.InitiateSystemShutdownA;
        real_InitiateSystemShutdownW   = (PFN_InitiateSystemShutdownW)g_host->real.InitiateSystemShutdownW;
        real_InitiateSystemShutdownExA = (PFN_InitiateSystemShutdownExA)g_host->real.InitiateSystemShutdownExA;
        real_InitiateSystemShutdownExW = (PFN_InitiateSystemShutdownExW)g_host->real.InitiateSystemShutdownExW;
    }
}

/* Called from the worker loop: re-read the ini.  Only state *changes* are
   logged (plus one ON line at startup, which is useful to confirm injection);
   the default "off" is not interesting and was printed by every process. */
static void SH_Tick(void)
{
    LONG was = g_shutEnabled;

    SH_ReadConfig();

    if (g_shutEnabled != was) {
        HookLog("[Guard] remote shutdown guard -> %s",
                g_shutEnabled ? "ON" : "off");
    } else if (!g_shutInitLogged) {
        g_shutInitLogged = 1;
        if (g_shutEnabled)
            HookLog("[Guard] remote shutdown guard -> ON (%s)", SH_INI_NAME);
    }
}
