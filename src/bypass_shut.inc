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

typedef BOOL      (WINAPI *PFN_ExitWindowsEx)(UINT, DWORD);
typedef UINT      (WINAPI *PFN_WinExec)(LPCSTR, UINT);
typedef HINSTANCE (WINAPI *PFN_ShellExecuteA)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
typedef HINSTANCE (WINAPI *PFN_ShellExecuteW)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);
typedef BOOL      (WINAPI *PFN_InitiateSystemShutdownA)(LPSTR, LPSTR, DWORD, BOOL, BOOL);
typedef BOOL      (WINAPI *PFN_InitiateSystemShutdownW)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL);
typedef BOOL      (WINAPI *PFN_InitiateSystemShutdownExA)(LPSTR, LPSTR, DWORD, BOOL, BOOL, DWORD);
typedef BOOL      (WINAPI *PFN_InitiateSystemShutdownExW)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL, DWORD);

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
    char   dir[MAX_PATH], path[MAX_PATH];
    char*  p;
    FILE*  f;
    char   line[128];
    LONG   enabled = 0;

    if (g_shutCfgTick && GetTickCount() - g_shutCfgTick < 500) return;
    g_shutCfgTick = GetTickCount();

    if (!GetModuleFileNameA(g_self, dir, sizeof(dir))) return;
    p = strrchr(dir, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(path, sizeof(path) - 1, "%s" SH_INI_NAME, dir);
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

    out[0] = 0;
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
    out[i] = 0;
    if (i > 4 && out[i - 4] == L'.' && out[i - 3] == L'e' &&
        out[i - 2] == L'x' && out[i - 1] == L'e')
        out[i - 4] = 0;
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

    wa[0] = 0;
    wc[0] = 0;
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
    if (SH_BlockA(NULL, cmd, "WinExec")) return 33;   /* > 31 = success */
    return real_WinExec ? real_WinExec(cmd, show) : 0;
}

static HINSTANCE WINAPI SH_Hook_ShellExecuteA(HWND owner, LPCSTR op, LPCSTR file,
                                              LPCSTR args, LPCSTR dir, INT show)
{
    if (SH_BlockA(file, args, "ShellExecuteA"))
        return (HINSTANCE)33;
    return real_ShellExecuteA ? real_ShellExecuteA(owner, op, file, args, dir, show)
                              : (HINSTANCE)0;
}

static HINSTANCE WINAPI SH_Hook_ShellExecuteW(HWND owner, LPCWSTR op, LPCWSTR file,
                                              LPCWSTR args, LPCWSTR dir, INT show)
{
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

/* One summary line per process instead of one per hooked API (the classroom
   spawns several of them; six lines each drowned the interesting entries). */
static void SH_LogHookSummary(const char* const* names, const BOOL* ok, int n)
{
    char list[256], failed[128];
    int  i, good = 0;

    list[0] = 0;
    failed[0] = 0;
    for (i = 0; i < n; i++) {
        char*  dst = ok[i] ? list : failed;
        size_t cap = ok[i] ? sizeof(list) : sizeof(failed);

        if (dst[0]) strncat(dst, " ", cap - strlen(dst) - 1);
        strncat(dst, names[i], cap - strlen(dst) - 1);
        if (ok[i]) good++;
    }

    if (good == n) {
        HookLog("[Guard] hooks installed: %s", list);
    } else {
        HookLog("[Guard] hooks %d/%d installed: %s", good, n, list);
        HookLog("[Guard] hook FAILED: %s", failed);
    }
}

static void SH_Init(void)
{
    HMODULE        mod;
    const char*    names[6];
    BOOL           ok[6];
    int            n = 0;
    FARPROC        p;

    SH_ReadConfig();

    mod = GetModuleHandleA("kernel32.dll");
    if (mod) {
        p = GetProcAddress(mod, "WinExec");
        if (p) {
            real_WinExec = (PFN_WinExec)p;
            names[n] = "WinExec";
            ok[n++] = InstallInlineHook((void*)p, (void*)SH_Hook_WinExec,
                                        (void**)&real_WinExec);
        }

        /* The 0x28 path runs `shutdown -s -t 1` through systemoper.dll, which
           is LoadLibrary'd on first use.  Its own IAT may never be patched in
           time, but the kernel32 entry is shared by every module in this
           process, so an entry hook covers it from the first call. */
        if (real_CreateProcessW) {
            p = GetProcAddress(mod, "CreateProcessW");
            if (p) {
                names[n] = "CreateProcessW";
                ok[n++] = InstallInlineHook((void*)p, (void*)Hook_CreateProcessW,
                                            (void**)&real_CreateProcessW);
            }
        }
        if (real_CreateProcessA) {
            p = GetProcAddress(mod, "CreateProcessA");
            if (p) {
                names[n] = "CreateProcessA";
                ok[n++] = InstallInlineHook((void*)p, (void*)Hook_CreateProcessA,
                                            (void**)&real_CreateProcessA);
            }
        }
    }

    mod = GetModuleHandleA("user32.dll");
    if (mod) {
        p = GetProcAddress(mod, "ExitWindowsEx");
        if (p) {
            real_ExitWindowsEx = (PFN_ExitWindowsEx)p;
            names[n] = "ExitWindowsEx";
            ok[n++] = InstallInlineHook((void*)p, (void*)SH_Hook_ExitWindowsEx,
                                        (void**)&real_ExitWindowsEx);
        }
    }

    mod = GetModuleHandleA("shell32.dll");
    if (mod) {
        p = GetProcAddress(mod, "ShellExecuteA");
        if (p) {
            real_ShellExecuteA = (PFN_ShellExecuteA)p;
            names[n] = "ShellExecuteA";
            ok[n++] = InstallInlineHook((void*)p, (void*)SH_Hook_ShellExecuteA,
                                        (void**)&real_ShellExecuteA);
        }
        p = GetProcAddress(mod, "ShellExecuteW");
        if (p) {
            real_ShellExecuteW = (PFN_ShellExecuteW)p;
            names[n] = "ShellExecuteW";
            ok[n++] = InstallInlineHook((void*)p, (void*)SH_Hook_ShellExecuteW,
                                        (void**)&real_ShellExecuteW);
        }
    }

    SH_LogHookSummary(names, ok, n);

    mod = GetModuleHandleA("advapi32.dll");
    if (mod) {
        p = GetProcAddress(mod, "InitiateSystemShutdownA");
        if (p) real_InitiateSystemShutdownA = (PFN_InitiateSystemShutdownA)p;
        p = GetProcAddress(mod, "InitiateSystemShutdownW");
        if (p) real_InitiateSystemShutdownW = (PFN_InitiateSystemShutdownW)p;
        p = GetProcAddress(mod, "InitiateSystemShutdownExA");
        if (p) real_InitiateSystemShutdownExA = (PFN_InitiateSystemShutdownExA)p;
        p = GetProcAddress(mod, "InitiateSystemShutdownExW");
        if (p) real_InitiateSystemShutdownExW = (PFN_InitiateSystemShutdownExW)p;
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
