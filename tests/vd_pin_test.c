/*
 * vd_pin_test.c -- regression test for the virtual-desktop pin (32-bit).
 *
 * What it proves
 *   A render window that lives on a virtual desktop the user is NOT looking at
 *   can be shown / raised / focused without the shell dragging the user over to
 *   that desktop.  That is the "re-broadcast pulls me back to desktop 1" bug.
 *
 * Modes
 *   (default)    load bypass_hook32.dll into this process, let it pin a
 *                "MultiRenderWindowClass" window to the desktop in
 *                bypass_vd.ini, then run the activation checks.  Desktop must
 *                not move and the foreground window must not change.
 *   --baseline   no DLL at all: the same window is moved to another desktop by
 *                hand, and the same calls are made.  This shows whether such
 *                calls really move the desktop in the first place (i.e. that
 *                the default mode is not vacuous).
 *
 * The desktop id of the foreground window is the current virtual desktop: the
 * foreground window always lives on the desktop being viewed.
 *
 * Build: make.bat vdtest
 * Run  : bin\vd_pin_test.exe [--baseline] [--dll <bypass_hook32.dll>]
 *
 * Exit code 0 = every check passed.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

/* ---------------- IVirtualDesktopManager (public, Win10 1607+) ------ */

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

static const GUID kClsid = {0xAA509086,0x5CA9,0x4C25,{0x8F,0x95,0x58,0x9D,0x3C,0x07,0xB4,0x8A}};
static const GUID kIid   = {0xA5CD92FF,0x29BE,0x454C,{0x8D,0x04,0xD8,0x28,0x79,0xFB,0x3F,0x1B}};

static VdObj* g_vd;
static int    g_fail;        /* pinned mode: something moved */
static int    g_movedAny;    /* any check saw the desktop move */
static int    g_expectMove;  /* baseline: moving is the point, not a failure */

/* the user's virtual desktop list, in the order Windows shows it */
static int RegList(GUID* list, int max)
{
    HKEY  k;
    DWORD type = 0, size;
    unsigned char buf[16 * 16 + 8];
    int   n = 0;
    DWORD i, off = 0;

    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    size = sizeof(buf);
    if (RegQueryValueExA(k, "VirtualDesktopIDs", NULL, &type, buf, &size) == ERROR_SUCCESS &&
        type == REG_BINARY && size >= 16) {
        if (size >= 20 && *(DWORD*)(void*)buf == (size - 4) / 16) off = 4;
        for (i = off; i + 16 <= size && n < max; i += 16, n++)
            memcpy(&list[n], buf + i, sizeof(GUID));
    }
    RegCloseKey(k);
    return n;
}

static void GuidStr(const GUID* g, char* out, int n)
{
    _snprintf(out, n - 1, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              (unsigned long)g->Data1, (unsigned)g->Data2, (unsigned)g->Data3,
              g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
              g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    out[n - 1] = 0;
}

static int GuidZero(const GUID* g)
{
    static const GUID z = {0,0,0,{0,0,0,0,0,0,0,0}};
    return IsEqualGUID(g, &z) ? 1 : 0;
}

static void Pump(DWORD ms)
{
    DWORD t0 = GetTickCount();
    MSG   m;
    while (GetTickCount() - t0 < ms) {
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageA(&m);
        }
        Sleep(20);
    }
}

/* desktop of a top level window; NULL window / failure -> zero guid */
static void DeskOf(HWND h, GUID* out)
{
    memset(out, 0, sizeof(*out));
    if (!h || !g_vd) return;
    if (FAILED(g_vd->v->GetWindowDesktopId(g_vd, h, out))) memset(out, 0, sizeof(*out));
}

/* the desktop the user is currently looking at */
static int CurDesktop(GUID* out)
{
    memset(out, 0, sizeof(*out));
    DeskOf(GetForegroundWindow(), out);
    return GuidZero(out) ? 0 : 1;
}

/* Print every desktop signal we know for one window: which desktop it is on
   (GetWindowDesktopId) and whether that is the desktop being viewed
   (IsWindowOnCurrentVirtualDesktop).  Used by --signals to find out which of
   the two is trustworthy for a hidden window. */
static void Signal(const char* label, HWND h)
{
    GUID g;
    BOOL onCur = -1;
    char s[64] = "?";
    HRESULT hr, hr2 = E_FAIL;

    memset(&g, 0, sizeof(g));
    if (!h) {
        printf("  %-18s <null>\n", label);
        return;
    }
    hr = g_vd->v->GetWindowDesktopId(g_vd, h, &g);
    hr2 = g_vd->v->IsWindowOnCurrentVirtualDesktop(g_vd, h, &onCur);
    GuidStr(&g, s, sizeof(s));
    printf("  %-18s hwnd=%p vis=%d id=%s hr=0x%08lX onCur=%d hr2=0x%08lX\n",
           label, (void*)h, IsWindowVisible(h), s,
           (unsigned long)hr, (int)onCur, (unsigned long)hr2);
}

/* Try to take the foreground from a process that does not own it.  A fresh
   process is normally not allowed to, so attach to the foreground thread's
   input queue first - that is the documented way round the foreground lock,
   and it is what makes the baseline run able to move the desktop at all. */
static int ForceForeground(HWND h)
{
    DWORD me = GetCurrentThreadId();
    DWORD fg = 0;
    HWND  fgw = GetForegroundWindow();

    if (fgw) fg = GetWindowThreadProcessId(fgw, NULL);
    if (fg) AttachThreadInput(me, fg, TRUE);
    SetForegroundWindow(h);
    if (fg) AttachThreadInput(me, fg, FALSE);
    Pump(400);
    return GetForegroundWindow() == h;
}

static void Report(const char* what, const GUID* before, HWND h)
{
    GUID  now;
    char  sb[64] = "?", sn[64] = "?";
    int   moved, fg;

    GuidStr(before, sb, sizeof(sb));
    moved = CurDesktop(&now) ? !IsEqualGUID(&now, before) : 0;
    GuidStr(&now, sn, sizeof(sn));

    printf("  %-40s desktop %s -> %s\n", what, sb, sn);
    if (moved) {
        g_movedAny = 1;
        if (!g_expectMove) g_fail = 1;
    }

    fg = (GetForegroundWindow() == h);
    printf("  %-40s %s%s\n", "",
           fg ? "window is foreground" : "foreground untouched",
           moved ? (g_expectMove ? "   (desktop moved)" : "   <-- DESKTOP MOVED (FAIL)")
                 : "");

    if (fg && !g_expectMove) {
        /* the pinned mode must not let a window on another desktop take focus */
        g_fail = 1;
    }
}

int main(int argc, char** argv)
{
    char    dll[MAX_PATH] = "bypass_hook32.dll";
    HMODULE dllMod;
    HWND    h, fg0;
    GUID    before, wd, list[16];
    char    s[64] = "?";
    int     i, n, baseline = 0, signals = 0;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--dll") && i + 1 < argc) {
            _snprintf(dll, sizeof(dll) - 1, "%s", argv[++i]);
        } else if (!_stricmp(argv[i], "--baseline")) {
            baseline = 1;
        } else if (!_stricmp(argv[i], "--signals")) {
            signals = 1;
            baseline = 1;   /* no DLL: we want the raw OS behaviour */
        }
    }
    g_expectMove = baseline;

    /* resolve the library next to this exe, so the test works from any cwd */
    {
        char dir[MAX_PATH];
        char* p;
        if (GetModuleFileNameA(NULL, dir, sizeof(dir)) && (p = strrchr(dir, '\\')) != NULL) {
            *(p + 1) = 0;
            if (!strchr(dll, '\\')) {
                char t[MAX_PATH];
                _snprintf(t, sizeof(t) - 1, "%s%s", dir, dll);
                _snprintf(dll, sizeof(dll) - 1, "%s", t);
            }
        }
    }

    printf("vd_pin_test: mode=%s\n", baseline ? "baseline (no hooks, premise check)"
                                              : "pinned (DLL loaded)");
    printf("             dll=%s\n", dll);

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) {
        printf("CoInitializeEx failed\n");
        return 2;
    }
    if (FAILED(CoCreateInstance(&kClsid, NULL, CLSCTX_INPROC_SERVER, &kIid, (void**)&g_vd)) || !g_vd) {
        printf("IVirtualDesktopManager unavailable\n");
        return 2;
    }

    n = RegList(list, 16);
    if (n < 2) {
        printf("need at least 2 virtual desktops (found %d)\n", n);
        return 2;
    }

    dllMod = baseline ? NULL : LoadLibraryA(dll);
    if (!baseline && !dllMod) {
        printf("LoadLibrary failed err=%lu\n", (unsigned long)GetLastError());
        return 2;
    }
    Pump(1200);          /* let the DLL resolve its target desktop */

    /* the renderer's class name: the pin and the window-mode logic key off it */
    {
        WNDCLASSA wc;
        memset(&wc, 0, sizeof(wc));
        wc.lpfnWndProc   = DefWindowProcA;
        wc.hInstance     = GetModuleHandleA(NULL);
        wc.lpszClassName = "MultiRenderWindowClass";
        wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        if (!RegisterClassA(&wc)) {
            printf("RegisterClassA failed err=%lu\n", (unsigned long)GetLastError());
            return 2;
        }
    }

    h = CreateWindowExA(0, "MultiRenderWindowClass", "vd pin test window",
                        WS_OVERLAPPEDWINDOW, 100, 100, 800, 600,
                        NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!h) {
        printf("CreateWindowExA failed err=%lu\n", (unsigned long)GetLastError());
        return 2;
    }
    printf("window=%p\n", (void*)h);

    if (signals) {
        char gs[64] = "?";
        GuidStr(&list[1], gs, sizeof(gs));
        printf("current desktop (from foreground, before moving anything):\n");
        Signal("foreground", GetForegroundWindow());
        Signal("Progman", FindWindowA("Progman", NULL));
        Signal("Shell_TrayWnd", FindWindowA("Shell_TrayWnd", NULL));
        Signal("new window", h);
        g_vd->v->MoveWindowToDesktop(g_vd, h, &list[1]);
        printf("after MoveWindowToDesktop(%s) while hidden:\n", gs);
        Signal("new window", h);
        ShowWindow(h, SW_SHOWNOACTIVATE);
        Pump(800);
        printf("after ShowWindow(SW_SHOWNOACTIVATE):\n");
        Signal("new window", h);
        Signal("foreground", GetForegroundWindow());
        ShowWindow(h, SW_HIDE);
        Pump(300);
        printf("after ShowWindow(SW_HIDE):\n");
        Signal("new window", h);
        Signal("foreground", GetForegroundWindow());
        DestroyWindow(h);
        return 0;
    }

    /* show it without activating (SW_SHOWNOACTIVATE cannot move the desktop by
       definition); GetWindowDesktopId() only reports a real id once shown */
    ShowWindow(h, SW_SHOWNOACTIVATE);
    Pump(1200);

    if (baseline) {
        /* Put the window on the second desktop by hand.  MoveWindowToDesktop()
           while the window is still hidden does not stick, so showing first and
           moving afterwards is deliberate. */
        char gs[64] = "?";
        GuidStr(&list[1], gs, sizeof(gs));
        if (FAILED(g_vd->v->MoveWindowToDesktop(g_vd, h, &list[1]))) {
            printf("baseline: MoveWindowToDesktop(%s) failed\n", gs);
            DestroyWindow(h);
            return 2;
        }
        Pump(600);
        printf("baseline: window moved to %s without any hook\n", gs);
    } else {
        Pump(1500);      /* the DLL sweep pins it to bypass_vd.ini's desktop */
    }

    DeskOf(h, &wd);
    GuidStr(&wd, s, sizeof(s));
    printf("window desktop=%s\n", s);

    if (!CurDesktop(&before)) {
        printf("cannot read the current desktop (no foreground window?)\n");
        DestroyWindow(h);
        return 2;
    }
    GuidStr(&before, s, sizeof(s));
    printf("current desktop=%s\n", s);

    if (GuidZero(&wd)) {
        printf("window desktop unknown - the pin never ran (bypass_vd.ini enabled=0?)\n");
        DestroyWindow(h);
        return 2;
    }
    if (IsEqualGUID(&wd, &before)) {
        if (baseline)
            printf("baseline: the window is on the current desktop - MoveWindowToDesktop\n"
                   "          did not take effect, the probe cannot continue\n");
        else
            printf("window is already on the current desktop - set bypass_vd.ini to a\n"
                   "NON-current desktop (desktop= index) and run again\n");
        DestroyWindow(h);
        return 2;
    }

    fg0 = GetForegroundWindow();
    printf("\nchecks (window is on another desktop, the user is not looking at it):\n");

    /* A. can this process take the foreground at all?  (baseline: it must, or
          every other check below is meaningless) */
    {
        int ok = ForceForeground(h);
        printf("  %-40s %s\n", "force foreground (attach + SetForegroundWindow)",
               ok ? "window became foreground" : "refused");
        if (!baseline && ok) g_fail = 1;
        if (baseline && !ok)
            printf("  %-40s <-- this build cannot activate windows, the probe is\n"
                   "  %-40s     inconclusive\n", "", "");
        Pump(300);
        Report("after force-foreground", &before, h);
    }

    /* B. show it again (the renderer's own SW_SHOW) */
    ShowWindow(h, SW_HIDE);
    Pump(300);
    ShowWindow(h, SW_SHOW);
    Pump(700);
    Report("after hide + ShowWindow(SW_SHOW)", &before, h);

    /* C. the renderer's 500ms topmost re-position: activating by default */
    SetWindowPos(h, HWND_TOPMOST, 100, 100, 800, 600, 0);
    Pump(700);
    Report("after SetWindowPos(HWND_TOPMOST, flags=0)", &before, h);

    /* D. focus it directly */
    SetForegroundWindow(h);
    Pump(700);
    Report("after SetForegroundWindow", &before, h);

    /* E. raise it */
    BringWindowToTop(h);
    Pump(700);
    Report("after BringWindowToTop", &before, h);

    /* self-heal: if something did move the user, bring the old window back */
    {
        GUID now;
        if (CurDesktop(&now) && !IsEqualGUID(&now, &before) && fg0) {
            printf("\nrestoring the user's desktop via hwnd=%p\n", (void*)fg0);
            ForceForeground(fg0);
            Pump(500);
        }
    }

    ShowWindow(h, SW_HIDE);
    DestroyWindow(h);

    if (baseline) {
        printf("\nprobe: activation %s the desktop.\n",
               g_movedAny ? "DOES move" : "did NOT move");
        printf("       (when it does not, the pinned mode cannot prove anything "
               "and the real cause is elsewhere)\n");
    } else {
        printf("\nnote: Windows usually refuses foreground changes from a "
               "process that does not own it,\n"
               "      so a refusal here is not proof on its own - the matching\n"
               "      \"vd: ...\" lines in bypass_hook.log show the hooks were "
               "consulted.\n");
    }
    printf("\nresult: %s\n", g_fail
           ? "FAIL - a window on another desktop took focus / moved the desktop"
           : "PASS - nothing was activated, the desktop never moved");
    return g_fail ? 1 : 0;
}
