/*
 * bypass_vd.inc -- keep the broadcast window on one Windows virtual desktop.
 *
 * Included from bypass_hook.c at file scope.  Pure ASCII on purpose: the
 * host file is GBK encoded and must not be re-encoded.
 *
 * Why this exists
 *   The classroom software only knows Win32 desktops (NetTeachingDesktop /
 *   MCLOCK Desktop), not Windows virtual desktops.  Every broadcast start
 *   spawns a new renderer process (ScreenRender.exe) whose window is created
 *   on whatever desktop is active at that moment.  Student on desktop 2 ->
 *   the new broadcast window pops up on desktop 2 and the shell may even
 *   follow it back to desktop 1.
 *
 * How
 *   1. CreateWindowExW hook: the render window is created hidden (dwStyle has
 *      no WS_VISIBLE; verified at ScreenRender.exe 0x414325 and 0x4143c9,
 *      class "MultiRenderWindowClass"), so pin it right after creation --
 *      it can never appear on the wrong desktop.
 *   2. ShowWindow / ShowWindowAsync / SetWindowPos(SWP_SHOWWINDOW): pin again
 *      right before it becomes visible.
 *   3. A 1 Hz sweep on a private thread as a safety net.
 *   4. Suppress activation of the render window (SetForegroundWindow /
 *      BringWindowToTop / SetActiveWindow, and in bypass_hook.c SWP_NOACTIVATE
 *      / SW_SHOWNOACTIVATE) while the pinned desktop is not the one the user is
 *      looking at, so the shell never drags the student back to it.
 *
 * Measured on this machine (Win10 19045, vdtest.exe / vdmove.exe)
 *   - MoveWindowToDesktop to a NON-current desktop returns S_OK.
 *   - GetWindowDesktopId returns {00000000-...} while the window is hidden;
 *     the move is still remembered and honoured at the first ShowWindow.
 *   - Works from a private STA thread while the owner thread is blocked, so
 *     all COM work happens on one dedicated thread (never in DllMain, never
 *     on the renderer's UI thread -> no apartment side effects).
 *   - A desktop id that no longer exists fails with 0x8002802B
 *     (ELEMENTNOTFOUND) -> fall back to the window's current desktop.
 *
 * Config: bypass_vd.ini next to the DLL (created on first run)
 *   enabled=1        0 disables the feature (old behaviour)
 *   desktop=0        index into the user's virtual desktop list, 0 = first
 *   guid={GUID}      optional explicit target, overrides desktop=
 *   pinned={GUID}    written by the tool: desktop actually in use
 */

#include <objbase.h>

#define VD_MAX_DESKTOPS 16
#define VD_INI_NAME     "bypass_vd.ini"
#define VD_REG_KEY      "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops"
#define VD_ERR_GONE     0x8002802BL   /* TYPE_E_ELEMENTNOTFOUND */

/* ---------------- public IVirtualDesktopManager ---------------- */
/* {AA509086-5CA9-4C25-8F95-589D3C07B48A} */
/* {A5CD92FF-29BE-454C-8D04-D82879FB3F1B} */

typedef struct VdObj VdObj;
typedef struct VdVtbl {
    HRESULT (WINAPI *QueryInterface)(VdObj*, const GUID*, void**);
    ULONG   (WINAPI *AddRef)(VdObj*);
    ULONG   (WINAPI *Release)(VdObj*);
    HRESULT (WINAPI *IsWindowOnCurrentVirtualDesktop)(VdObj*, HWND, BOOL*);
    HRESULT (WINAPI *GetWindowDesktopId)(VdObj*, HWND, GUID*);
    HRESULT (WINAPI *MoveWindowToDesktop)(VdObj*, HWND, const GUID*);
} VdVtbl;

struct VdObj { const VdVtbl* v; };

static const GUID g_vdClsid = {0xAA509086,0x5CA9,0x4C25,{0x8F,0x95,0x58,0x9D,0x3C,0x07,0xB4,0x8A}};
static const GUID g_vdIid   = {0xA5CD92FF,0x29BE,0x454C,{0x8D,0x04,0xD8,0x28,0x79,0xFB,0x3F,0x1B}};

/* ---------------- state ---------------- */

/* owned by the VD thread only */
static VdObj* g_vdObj;
static GUID   g_vdTarget;
static int    g_vdTargetValid;
static GUID   g_vdPinned;          /* persisted in bypass_vd.ini */
static int    g_vdPinnedValid;
static int    g_vdIndex;           /* desktop= from the ini */
static int    g_vdCfgEnabled = 1;
static char   g_vdCfgGuid[64];

/* shared with the hooks */
static volatile LONG g_vdStarted;
static volatile LONG g_vdRunning;
static HANDLE        g_vdThread;
static volatile LONG g_vdEnabled = 1;
static volatile LONG g_vdBusy;
static volatile LONG g_vdLogs;
static volatile LONG g_vdFailLogs;
static volatile LONG g_vdConfigLogs;
static volatile LONG g_vdDenied;   /* E_ACCESSDENIED: window is not on the input desktop */
static HANDLE g_vdReq;
static HANDLE g_vdAck;
static CRITICAL_SECTION g_vdCs;
static HWND   g_vdSlot;
static HWND   g_vdWnd;             /* render window seen last by the VD thread */
/* 1 = the target desktop is the one the user is looking at right now.
   Maintained on the VD thread only (COM calls stay off the renderer thread).
   Everything that would activate the render window while this is 0 is
   suppressed - that is what stops the shell from walking the student back to
   the pinned desktop on every new broadcast.
   NOTE: default 0 = "suppress".  A freshly injected process has no render window
   of its own yet, and the foreground window may still be a window that lives
   on the pinned desktop (the user switched to an empty desktop, where Windows
   keeps the previous foreground window).  Guessing "current" in that state is
   exactly what let the first ShowWindow(SW_SHOW) of a new broadcast drag the
   student over, so the unknown state must be the safe one. */
static volatile LONG g_vdTargetIsCur = 0;
static volatile LONG g_vdInit;     /* VD thread finished its init (ok or not) */
static char   g_vdPath[MAX_PATH];

static void VD_Start(void);
static void VD_PinNow(HWND h, const char* why);
static void VD_Track(HWND h);
static int  VD_PinOne(HWND h);
static void VD_RefreshTargetIsCur(void);

/* ---------------- helpers ---------------- */

static int VD_GuidIsZero(const GUID* g)
{
    static const GUID zero = {0,0,0,{0,0,0,0,0,0,0,0}};
    return IsEqualGUID(g, &zero) ? 1 : 0;
}

static void VD_GuidToStr(const GUID* g, char* out, int n)
{
    _snprintf(out, n - 1,
              "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              (unsigned long)g->Data1, (unsigned)g->Data2, (unsigned)g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
              g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    out[n - 1] = 0;
}

static int VD_StrToGuid(const char* s, GUID* g)
{
    unsigned long d1;
    unsigned d2, d3, b[8];
    int i;

    if (!s || !*s) return 0;
    if (sscanf(s, "{%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
               &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3],
               &b[4], &b[5], &b[6], &b[7]) != 11)
        return 0;
    g->Data1 = d1;
    g->Data2 = (unsigned short)d2;
    g->Data3 = (unsigned short)d3;
    for (i = 0; i < 8; i++) g->Data4[i] = (unsigned char)b[i];
    return 1;
}

static char* VD_Trim(char* s)
{
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}

/* real user32 export, never our own IAT slot */
static FARPROC VD_User32Proc(const char* name)
{
    HMODULE u = GetModuleHandleA("user32.dll");
    if (!u) u = LoadLibraryA("user32.dll");
    return u ? GetProcAddress(u, name) : NULL;
}

/* The pin only ever touches the render window itself.  Class based (the log
   shows cls=MultiRenderWindowClass in ScreenRender.exe), top level only. */
static BOOL VD_IsRenderWindow(HWND h)
{
    char cls[96];

    if (!h || !IsWindow(h)) return FALSE;
    if (GetAncestor(h, GA_ROOT) != h) return FALSE;
    cls[0] = 0;
    GetClassNameA(h, cls, sizeof(cls));
    return IsRenderWindowClass(cls);
}

/* ---------------- log throttling ---------------- */

/* ScreenRender calls ShowWindow() ~10x per second and SetWindowPos() just as
   often.  A log line per rewritten call buries everything else, so each
   (window, action) pair is reported once and then at most once per VD_LOG_MS. */
#define VD_LOG_SLOTS 16
#define VD_LOG_MS    15000

enum { VD_LOG_SHOW = 1, VD_LOG_SHOWASYNC, VD_LOG_SWP, VD_LOG_PLACEMENT,
       VD_LOG_FG, VD_LOG_BRING, VD_LOG_ACTIVE, VD_LOG_STATE };

typedef struct { HWND h; int act; DWORD tick; } VD_LOGSLOT;
static VD_LOGSLOT g_vdLogTab[VD_LOG_SLOTS];

static int VD_LogWanted(HWND h, int act, DWORD ms)
{
    DWORD now = GetTickCount();
    int   i, freeSlot = -1;

    for (i = 0; i < VD_LOG_SLOTS; i++) {
        if (g_vdLogTab[i].h == h && g_vdLogTab[i].act == act) {
            if ((DWORD)(now - g_vdLogTab[i].tick) < ms) return 0;
            g_vdLogTab[i].tick = now;
            return 1;
        }
        if (!g_vdLogTab[i].act && freeSlot < 0) freeSlot = i;
    }
    if (freeSlot < 0) return 1;        /* table full: keep the evidence */
    g_vdLogTab[freeSlot].h = h;
    g_vdLogTab[freeSlot].act = act;
    g_vdLogTab[freeSlot].tick = now;
    return 1;
}

/* ---------------- bypass_vd.ini ---------------- */

static void VD_BuildPath(void)
{
    char dir[MAX_PATH];
    char* p;

    if (g_vdPath[0] || !g_dllPath[0]) return;
    _snprintf(dir, sizeof(dir) - 1, "%s", g_dllPath);
    dir[sizeof(dir) - 1] = 0;
    p = strrchr(dir, '\\');
    if (!p) return;
    *(p + 1) = 0;
    _snprintf(g_vdPath, sizeof(g_vdPath) - 1, "%s%s", dir, VD_INI_NAME);
    g_vdPath[sizeof(g_vdPath) - 1] = 0;
}

static void VD_LoadIni(void)
{
    FILE* f;
    char  line[256];

    g_vdCfgEnabled = 1;
    g_vdIndex = 0;
    g_vdCfgGuid[0] = 0;

    if (!g_vdPath[0]) return;
    f = fopen(g_vdPath, "r");
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        char* eq;
        char* k = VD_Trim(line);
        char* v;

        if (!*k || *k == '#' || *k == ';' || *k == '[') continue;
        eq = strchr(k, '=');
        if (!eq) continue;
        *eq = 0;
        v = VD_Trim(eq + 1);
        k = VD_Trim(k);

        if (!_stricmp(k, "enabled")) {
            g_vdCfgEnabled = atoi(v) ? 1 : 0;
        } else if (!_stricmp(k, "desktop")) {
            g_vdIndex = atoi(v);
            if (g_vdIndex < 0) g_vdIndex = 0;
        } else if (!_stricmp(k, "guid")) {
            _snprintf(g_vdCfgGuid, sizeof(g_vdCfgGuid) - 1, "%s", v);
            g_vdCfgGuid[sizeof(g_vdCfgGuid) - 1] = 0;
        } else if (!_stricmp(k, "pinned")) {
            GUID g;
            if (VD_StrToGuid(v, &g)) {
                g_vdPinned = g;
                g_vdPinnedValid = 1;
            }
        }
    }
    fclose(f);
}

static void VD_WriteIni(void)
{
    FILE* f;
    char  s[64] = "";

    if (!g_vdPath[0]) return;
    /* binary mode: the lines below already carry explicit CRLF, text mode
       would turn each of them into CR CR LF */
    f = fopen(g_vdPath, "wb");
    if (!f) return;

    fprintf(f, "; bypass_hook - keep the broadcast window on one virtual desktop\r\n");
    fprintf(f, "; enabled=0 : old behaviour (window follows the active desktop)\r\n");
    fprintf(f, "; desktop   : index in the Windows virtual desktop list, 0 = first\r\n");
    fprintf(f, "; guid      : optional explicit {GUID}, overrides desktop=\r\n");
    fprintf(f, "enabled=%d\r\n", g_vdCfgEnabled);
    fprintf(f, "desktop=%d\r\n", g_vdIndex);
    fprintf(f, "guid=%s\r\n", g_vdCfgGuid);
    if (g_vdPinnedValid) VD_GuidToStr(&g_vdPinned, s, sizeof(s));
    fprintf(f, "pinned=%s\r\n", s);
    fclose(f);
}

/* ---------------- virtual desktop list ---------------- */

static int VD_RegList(GUID* list, int max)
{
    HKEY  k;
    DWORD type = 0;
    DWORD size = 0;
    unsigned char buf[16 * VD_MAX_DESKTOPS + 8];
    int   n = 0;
    DWORD i;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, VD_REG_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;

    size = sizeof(buf);
    if (RegQueryValueExA(k, "VirtualDesktopIDs", NULL, &type, buf, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size >= 16) {
        DWORD off = 0;
        /* some builds prefix the blob with a DWORD count */
        if (size >= 20 && *(DWORD*)(void*)buf == (size - 4) / 16) off = 4;
        for (i = off; i + 16 <= size && n < max; i += 16, n++)
            memcpy(&list[n], buf + i, sizeof(GUID));
    }
    RegCloseKey(k);
    return n;
}

/* Pick the desktop to pin to: explicit guid, else saved file (if it still
   exists), else desktop[index] of the user's list. */
static void VD_ResolveTarget(void)
{
    GUID list[VD_MAX_DESKTOPS];
    char s[64] = "?";
    int  n, i, idx;

    if (g_vdCfgGuid[0] && VD_StrToGuid(g_vdCfgGuid, &g_vdTarget)) {
        g_vdTargetValid = 1;
        if (g_isBroadcastProc) {
            VD_GuidToStr(&g_vdTarget, s, sizeof(s));
            HookLog("vd: target from bypass_vd.ini guid=%s", s);
        }
        return;
    }

    n = VD_RegList(list, VD_MAX_DESKTOPS);

    if (g_vdPinnedValid) {
        int keep = (n == 0);
        for (i = 0; i < n; i++)
            if (IsEqualGUID(&list[i], &g_vdPinned)) { keep = 1; break; }
        if (keep) {
            g_vdTarget = g_vdPinned;
            g_vdTargetValid = 1;
            if (g_isBroadcastProc) {
                VD_GuidToStr(&g_vdTarget, s, sizeof(s));
                HookLog("vd: target from saved state guid=%s (%d desktop(s))", s, n);
            }
            return;
        }
        g_vdPinnedValid = 0;
        HookLog("vd: saved desktop is gone - re-resolving");
    }

    if (n > 0) {
        idx = g_vdIndex;
        if (idx >= n) idx = n - 1;
        g_vdTarget = list[idx];
        g_vdTargetValid = 1;
        g_vdPinned = g_vdTarget;
        g_vdPinnedValid = 1;
        VD_WriteIni();
        if (g_isBroadcastProc) {
            VD_GuidToStr(&g_vdTarget, s, sizeof(s));
            HookLog("vd: target = desktop[%d] = %s (%d desktop(s) in registry)", idx, s, n);
        }
        return;
    }

    if (InterlockedIncrement(&g_vdConfigLogs) <= 3)
        HookLog("vd: no virtual desktop list in registry - will adopt the render window's desktop");
}

static void VD_Track(HWND h)
{
    g_vdWnd = h;
}

/* Is the pinned desktop the one the user is looking at right now?
 *
 * The shell switches virtual desktops when a window on another desktop gets
 * activated, so this is the flag every activation path consults.  Two
 * sources, both validated with IsWindowOnCurrentVirtualDesktop():
 *
 *   1. our own render window - it lives on the target desktop, so
 *      IsWindowOnCurrentVirtualDesktop() answers exactly "is the target the
 *      desktop being viewed".  Only usable while it is visible: for a hidden
 *      window the API always reports "yes" once and GetWindowDesktopId()
 *      returns a zero GUID (measured with bin\vd_pin_test.exe --signals).
 *   2. the foreground window - its desktop id plus the same yes/no check.
 *      The id alone is NOT enough: when the viewed desktop has no windows,
 *      Windows can keep the previous foreground window, which may sit on the
 *      pinned desktop, and the id would then wrongly claim "user is here".
 *
 * Unknown keeps the last value; the initial value is 0 (suppress), so a new
 * process can never let the first broadcast show/activation move the user.
 */
static void VD_RefreshTargetIsCur(void)
{
    int res = -1;           /* -1 = unknown */

    if (!g_vdObj || !g_vdTargetValid) return;

    if (g_vdWnd && IsWindow(g_vdWnd) && IsWindowVisible(g_vdWnd)) {
        GUID w;
        memset(&w, 0, sizeof(w));
        if (SUCCEEDED(g_vdObj->v->GetWindowDesktopId(g_vdObj, g_vdWnd, &w)) &&
            !VD_GuidIsZero(&w) && IsEqualGUID(&w, &g_vdTarget)) {
            BOOL onCur = FALSE;
            if (SUCCEEDED(g_vdObj->v->IsWindowOnCurrentVirtualDesktop(g_vdObj, g_vdWnd, &onCur)))
                res = onCur ? 1 : 0;
        }
    }

    if (res < 0) {
        HWND fg = GetForegroundWindow();
        if (fg && IsWindow(fg)) {
            GUID g;
            BOOL onCur = FALSE;
            memset(&g, 0, sizeof(g));
            if (SUCCEEDED(g_vdObj->v->GetWindowDesktopId(g_vdObj, fg, &g)) &&
                !VD_GuidIsZero(&g) &&
                SUCCEEDED(g_vdObj->v->IsWindowOnCurrentVirtualDesktop(g_vdObj, fg, &onCur)))
                res = (IsEqualGUID(&g, &g_vdTarget) && onCur) ? 1 : 0;
        }
    }

    if (res < 0) return;                       /* keep the last known value */
    if ((g_vdTargetIsCur != 0) == (res != 0)) return;

    InterlockedExchange(&g_vdTargetIsCur, res);
    /* A user switching back and forth must not fill the log; the state itself
       still flips every time, only the line is rate limited. */
    if (VD_LogWanted(NULL, VD_LOG_STATE, 3000))
        HookLog(res ? "vd: user is back on the pinned desktop"
                    : "vd: user left the pinned desktop - render window activation suppressed");
}

/* The hooks can run before the VD thread has finished resolving the target
   (a fresh process creates its render window within milliseconds).  Without
   this the very first ShowWindow would be judged before the state exists. */
static void VD_WaitReady(DWORD ms)
{
    DWORD t0 = GetTickCount();

    while (!g_vdInit && g_vdStarted && (DWORD)(GetTickCount() - t0) < ms)
        Sleep(5);
}

/* Move one window to the pinned desktop.  Returns 1 when it ends up there. */static int VD_PinOne(HWND h)
{
    GUID    cur;
    HRESULT hr;
    char    s[64] = "?";
    int     isZero = 1;

    if (!g_vdObj || !VD_IsRenderWindow(h)) return 0;

    memset(&cur, 0, sizeof(cur));
    hr = g_vdObj->v->GetWindowDesktopId(g_vdObj, h, &cur);
    isZero = VD_GuidIsZero(&cur);

    if (!g_vdTargetValid) {
        if (SUCCEEDED(hr) && !isZero) {
            g_vdTarget = cur;
            g_vdTargetValid = 1;
            g_vdPinned = cur;
            g_vdPinnedValid = 1;
            VD_WriteIni();
            VD_GuidToStr(&cur, s, sizeof(s));
            HookLog("vd: adopted desktop=%s (first render window)", s);
        } else {
            return 0;   /* hidden window and no list: wait for the sweep */
        }
    }

    if (SUCCEEDED(hr) && !isZero && IsEqualGUID(&cur, &g_vdTarget)) {
        VD_Track(h);
        VD_RefreshTargetIsCur();
        return 1;
    }

    /* Moving the *foreground* window to a desktop the user is not looking at
       makes the shell follow the window (the user's view follows it too).
       With the suppression above this should not happen any more; keep the
       line so the log can prove it if it ever does again. */
    if (GetForegroundWindow() == h && InterlockedIncrement(&g_vdLogs) <= 40)
        HookLog("vd: moving the foreground window hwnd=%p - the shell may follow it",
                (void*)h);

    hr = g_vdObj->v->MoveWindowToDesktop(g_vdObj, h, &g_vdTarget);
    if (SUCCEEDED(hr)) {
        VD_Track(h);
        VD_RefreshTargetIsCur();
        if (InterlockedIncrement(&g_vdLogs) <= 40) {
            VD_GuidToStr(&g_vdTarget, s, sizeof(s));
            HookLog("vd: pinned hwnd=%p -> %s (targetIsCurrent=%d visible=%d wasHiddenId=%d)",
                    (void*)h, s, g_vdTargetIsCur, IsWindowVisible(h), isZero);
        }
        return 1;
    }

    if (hr == E_ACCESSDENIED) {
        /* The window lives on a non-input Win32 desktop: the classroom suite
           switches the session to NetTeachingDesktop / "MCLOCK Desktop", and
           IVirtualDesktopManager refuses to touch windows that are not on the
           input desktop.  Nothing can be fixed from in here, and retrying at
           1 Hz would only flood the log - disable the pin for this process. */
        if (InterlockedCompareExchange(&g_vdDenied, 1, 0) == 0)
            HookLog("vd: pinning disabled - hwnd=%p is on a non-interactive Win32 "
                    "desktop (hr=0x%08lX); the renderer has to be created on the "
                    "input desktop for the pin to work",
                    (void*)h, (unsigned long)hr);
        return 0;
    }

    if ((unsigned long)hr == VD_ERR_GONE || hr == E_INVALIDARG) {
        /* the pinned desktop was closed: fall back to where the window is */
        if (!isZero) {
            g_vdTarget = cur;
            g_vdTargetValid = 1;
            g_vdPinned = cur;
            g_vdPinnedValid = 1;
            VD_WriteIni();
            VD_GuidToStr(&cur, s, sizeof(s));
            HookLog("vd: target desktop no longer exists - fallback to %s", s);
            return 0;
        }
    }
    if (InterlockedIncrement(&g_vdFailLogs) <= 10)
        HookLog("vd: move failed hwnd=%p hr=0x%08lX", (void*)h, (unsigned long)hr);
    return 0;
}

static BOOL CALLBACK VD_EnumProc(HWND h, LPARAM p)
{
    DWORD pid = 0;

    (void)p;
    GetWindowThreadProcessId(h, &pid);
    if (pid != g_pid) return TRUE;
    if (!IsWindowVisible(h)) return TRUE;
    if (!VD_IsRenderWindow(h)) return TRUE;
    VD_PinOne(h);
    return TRUE;
}

static void VD_Sweep(void)
{
    if (!g_vdObj || g_vdDenied) return;
    EnumWindows(VD_EnumProc, 0);
}

static DWORD WINAPI VD_Thread(LPVOID p)
{
    DWORD lastSweep = GetTickCount();
    DWORD lastCfg   = GetTickCount();
    HRESULT hr;

    (void)p;

    hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        HookLog("vd: CoInitializeEx failed 0x%08lX - pin disabled", (unsigned long)hr);
        InterlockedExchange(&g_vdEnabled, 0);
        InterlockedExchange(&g_vdInit, 1);
        return 0;
    }

    VD_BuildPath();
    VD_LoadIni();
    if (g_vdPath[0] && GetFileAttributesA(g_vdPath) == INVALID_FILE_ATTRIBUTES)
        VD_WriteIni();   /* first run: leave a self-documenting config */

    hr = CoCreateInstance(&g_vdClsid, NULL, CLSCTX_INPROC_SERVER,
                          &g_vdIid, (void**)&g_vdObj);
    if (FAILED(hr) || !g_vdObj) {
        g_vdObj = NULL;
        HookLog("vd: IVirtualDesktopManager not available (hr=0x%08lX) - pin disabled",
                (unsigned long)hr);
        InterlockedExchange(&g_vdEnabled, 0);
        InterlockedExchange(&g_vdInit, 1);
        return 0;
    }

    HookLog("vd: thread started (p2b-1 marker)");

    if (g_vdCfgEnabled) {
        char ts[64] = "-";

        VD_ResolveTarget();
        /* Resolve the "is the target the current desktop" state before any
           hook can ask: unknown counts as "no" (suppression on). */
        VD_RefreshTargetIsCur();
        if (g_vdTargetValid) VD_GuidToStr(&g_vdTarget, ts, sizeof(ts));

        /* One line per injected process instead of three: the interesting
           part is only whether this process will suppress or not. */
        if (g_isBroadcastProc || !g_vdTargetIsCur)
            HookLog("vd: pinning on desktop=%d target=%s targetIsCurrent=%d "
                    "(activation %s)", g_vdIndex, ts, g_vdTargetIsCur,
                    g_vdTargetIsCur ? "allowed" : "suppressed");
    } else {
        if (g_isBroadcastProc)
            HookLog("vd: pinning off (bypass_vd.ini enabled=0)");
    }
    InterlockedExchange(&g_vdEnabled, g_vdCfgEnabled ? 1 : 0);
    InterlockedExchange(&g_vdInit, 1);

    for (;;) {
        HWND  h = NULL;
        DWORD w;

        if (InterlockedCompareExchange(&g_vdRunning, 0, 0) == 0) break;
        w = WaitForSingleObject(g_vdReq, 500);

        if (w == WAIT_OBJECT_0) {
            EnterCriticalSection(&g_vdCs);
            h = g_vdSlot;
            g_vdSlot = NULL;
            LeaveCriticalSection(&g_vdCs);
        }

        if (GetTickCount() - lastCfg >= 5000) {
            char oldGuid[64];
            int  oldIdx = g_vdIndex;

            lastCfg = GetTickCount();
            memcpy(oldGuid, g_vdCfgGuid, sizeof(oldGuid));
            VD_LoadIni();
            if (strcmp(oldGuid, g_vdCfgGuid) != 0 || oldIdx != g_vdIndex) {
                g_vdTargetValid = 0;
                if (g_vdCfgEnabled) VD_ResolveTarget();
            }
            InterlockedExchange(&g_vdEnabled, g_vdCfgEnabled ? 1 : 0);
        }

        if (g_vdEnabled && !g_disabled && !g_vdDenied)
            VD_RefreshTargetIsCur();      /* drives the activation suppression */

        if (h) {
            if (g_vdEnabled && !g_disabled && !g_vdDenied) VD_PinOne(h);
            SetEvent(g_vdAck);
        }

        if (g_vdEnabled && !g_disabled && !g_vdDenied &&
            GetTickCount() - lastSweep >= 1000) {
            lastSweep = GetTickCount();
            VD_Sweep();
        }
    }
    /* P2b: reached when VD_StopWait sets g_vdRunning = 0 */
    if (g_vdObj) {
        g_vdObj->v->Release(g_vdObj);
        g_vdObj = NULL;
    }
    CoUninitialize();
    InterlockedExchange(&g_vdInit, 1);
    return 0;
}

static void VD_Start(void)
{
    HANDLE th;

    if (InterlockedCompareExchange(&g_vdStarted, 1, 0) != 0) return;

    /* Every early-out below MUST set g_vdInit: VD_WaitReady() spins (up to
       500ms per call, on the caller's thread) while g_vdStarted is set and
       g_vdInit is not.  The teacher's preview window IS a render window, so
       without this every ShowWindow/SetWindowPos there would stall half a
       second - for the lifetime of the process. */
    if (IsTeacherSideImage(g_ownImage)) {
        HookLog("vd: teacher-side process - pin skipped");
        InterlockedExchange(&g_vdEnabled, 0);
        InterlockedExchange(&g_vdInit, 1);
        return;
    }

    VD_BuildPath();
    InitializeCriticalSection(&g_vdCs);
    g_vdReq = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_vdAck = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_vdReq || !g_vdAck) {
        HookLog("vd: event creation failed (err=%lu) - pin disabled",
                (unsigned long)GetLastError());
        InterlockedExchange(&g_vdEnabled, 0);
        InterlockedExchange(&g_vdInit, 1);
        return;
    }
    InterlockedExchange(&g_vdRunning, 1);
    th = CreateThread(NULL, 0, VD_Thread, NULL, 0, NULL);
    if (th) g_vdThread = th;      /* kept for VD_StopWait (P2b) */
    else {
        HookLog("vd: worker thread failed (err=%lu)", (unsigned long)GetLastError());
        InterlockedExchange(&g_vdEnabled, 0);
        InterlockedExchange(&g_vdInit, 1);
    }
}

/* P2b: stop the VD thread and wait.  1 = stopped (or never started). */
static int VD_StopWait(DWORD ms)
{
    DWORD w;

    InterlockedExchange(&g_vdRunning, 0);
    if (!g_vdThread) return 1;
    w = WaitForSingleObject(g_vdThread, ms);
    if (w != WAIT_OBJECT_0)
        return 0;                  /* handle kept: the next reload waits again /
                                      phase 1 refuses while "already stopping" */
    CloseHandle(g_vdThread);
    g_vdThread = NULL;
    return 1;
}

/* Called from the hooks: hand the window to the VD thread and wait briefly so
   the move really happens *before* the caller shows it. */
static void VD_PinNow(HWND h, const char* why)
{
    DWORD w;

    if (!h || g_disabled) return;
    if (!VD_IsRenderWindow(h)) return;
    if (g_vdDenied) return;                 /* known impossible in this process */
    if (!g_vdStarted) VD_Start();
    VD_WaitReady(500);                      /* fresh process: let the target resolve */
    if (!g_vdEnabled || !g_vdReq || !g_vdAck) return;
    if (IsIconic(h)) return;
    if (InterlockedCompareExchange(&g_vdBusy, 1, 0) != 0) return;

    ResetEvent(g_vdAck);
    EnterCriticalSection(&g_vdCs);
    g_vdSlot = h;
    LeaveCriticalSection(&g_vdCs);
    SetEvent(g_vdReq);
    w = WaitForSingleObject(g_vdAck, 250);
    if (w != WAIT_OBJECT_0 && InterlockedIncrement(&g_vdFailLogs) <= 5)
        HookLog("vd: pin request timeout hwnd=%p (%s)", (void*)h, why ? why : "?");
    InterlockedExchange(&g_vdBusy, 0);
}

/* 1 = this window must not be activated: it sits on a virtual desktop the user
   is not looking at, and activating it is exactly what makes the shell jump to
   that desktop ("a new broadcast drags me back to desktop 1").
   Class based and cross-process on purpose: MultiClient activates the window
   owned by ScreenRender.exe. */
static BOOL VD_NoActivate(HWND h)
{
    if (!h || g_disabled || !g_vdEnabled) return FALSE;
    if (g_vdDenied || !g_vdTargetValid) return FALSE;
    if (g_vdTargetIsCur) return FALSE;
    return VD_IsRenderWindow(h) ? TRUE : FALSE;
}

/* ---------------- hooks ---------------- */

static HWND WINAPI Hook_CreateWindowExW(DWORD exStyle, LPCWSTR cls, LPCWSTR name,
                                        DWORD style, int x, int y, int w, int h,
                                        HWND parent, HMENU menu, HINSTANCE inst,
                                        LPVOID param)
{
    typedef HWND (WINAPI *PFN)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                               HWND, HMENU, HINSTANCE, LPVOID);
    static PFN real;
    HWND hw;

    if (!real) real = (PFN)(void*)VD_User32Proc("CreateWindowExW");
    if (!real) return NULL;

    hw = real(exStyle, cls, name, style, x, y, w, h, parent, menu, inst, param);
    /* Hide the 0x28 countdown window (class=ShutDownDlg) as soon as it is
       created: shutdown is already blocked, and a visible "shutting down in N
       seconds" dialog only makes the tool look broken. */
    if (hw && SH_SuppressDialog(hw, cls)) return hw;
    /* still hidden here (dwStyle has no WS_VISIBLE in ScreenRender.exe), so
       moving it now means it can never flash on the wrong desktop.
       CORE_DISABLED: the dialog hider above is shutdown safety (keep it);
       the desktop pinning is window logic (pass through). */
    if (hw && !CORE_DISABLED() && !g_disabled) VD_PinNow(hw, "create");
    return hw;
}

static BOOL WINAPI Hook_SetForegroundWindow(HWND h)
{
    typedef BOOL (WINAPI *PFN)(HWND);
    static PFN real;

    if (!real) real = (PFN)(void*)VD_User32Proc("SetForegroundWindow");
    if (CORE_DISABLED()) return real ? real(h) : FALSE;
    if (!g_disabled && VD_NoActivate(h)) {
        if (VD_LogWanted(h, VD_LOG_FG, VD_LOG_MS))
            HookLog("vd: suppressed SetForegroundWindow(hwnd=%p) - pinned desktop "
                    "is not the current one", (void*)h);
        return TRUE;
    }
    return real ? real(h) : FALSE;
}

static BOOL WINAPI Hook_BringWindowToTop(HWND h)
{
    typedef BOOL (WINAPI *PFN)(HWND);
    static PFN real;

    if (!real) real = (PFN)(void*)VD_User32Proc("BringWindowToTop");
    if (CORE_DISABLED()) return real ? real(h) : FALSE;
    if (!g_disabled && VD_NoActivate(h)) {
        if (VD_LogWanted(h, VD_LOG_BRING, VD_LOG_MS))
            HookLog("vd: suppressed BringWindowToTop(hwnd=%p)", (void*)h);
        return TRUE;
    }
    return real ? real(h) : FALSE;
}

static HWND WINAPI Hook_SetActiveWindow(HWND h)
{
    typedef HWND (WINAPI *PFN)(HWND);
    static PFN real;

    if (!real) real = (PFN)(void*)VD_User32Proc("SetActiveWindow");
    if (CORE_DISABLED()) return real ? real(h) : NULL;
    if (!g_disabled && VD_NoActivate(h)) {
        if (VD_LogWanted(h, VD_LOG_ACTIVE, VD_LOG_MS))
            HookLog("vd: suppressed SetActiveWindow(hwnd=%p)", (void*)h);
        return NULL;
    }
    return real ? real(h) : NULL;
}
