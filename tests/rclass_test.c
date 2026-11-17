/* ============================================================
 *  rclass_test.c -- contract test for IsRenderWindowClass()
 *
 *  The DLL decides "this is a classroom render window" from the window
 *  CLASS NAME PREFIX alone: cls starts with "MultiRender", "StreamRenderWindow",
 *  "OeScreenRenderWindow", or equals "SDL_app".  It never looks at the owning
 *  process.  That is deliberate (the vendor's class names carry a build suffix
 *  we do not control), and it has a cost: any third-party window whose class
 *  starts with MultiRender is treated as a broadcast window -- its
 *  HWND_TOPMOST requests are rewritten to HWND_NOTOPMOST, its geometry is
 *  frozen, its style is rewritten.
 *
 *  Both windows below belong to THIS process, which is rclass_test.exe -- not
 *  ScreenRender.exe / MultiRender.exe / SharedDesktop.exe.  So:
 *
 *    MultiRenderContractProbe   topmost must be STRIPPED   (prefix hit)
 *    PlainContractProbe         topmost must be KEPT       (no prefix)
 *
 *  Run without --inject first (baseline: nothing is stripped), then with it.
 *  If someone tightens the match to an exact class name or adds an image
 *  check, the injected run fails here -- which is the point: that is a
 *  behaviour change, not a bug fix, and it must not happen silently.
 *
 *  用法: rclass_test.exe [--baseline] [--inject <bypass_hook32.dll>] [<inject32.exe>]
 *  Exit code: 0 = both windows behaved as documented, 1 = contract broken,
 *             2 = setup failure.
 * ============================================================ */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/* Mixed case on purpose: the match is case-insensitive. */
#define RCLASS "MultiRenderContractProbe"
#define PCLASS "PlainContractProbe"

static HWND g_render;
static HWND g_plain;

static void Say(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(h, msg, wp, lp);
}

static BOOL RegisterOne(const char* name)
{
    WNDCLASSA wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = name;

    SetLastError(0);
    if (!RegisterClassA(&wc)) {
        DWORD e = GetLastError();
        if (e != ERROR_CLASS_ALREADY_EXISTS) {
            Say("RegisterClassA(%s) failed err=%lu", name, (unsigned long)e);
            return FALSE;
        }
    }
    return TRUE;
}

/* The DLL posts its fix back to the window thread, and WM_WINDOWPOSCHANGING
   only exists while a message is being dispatched, so Sleep is not enough. */
static void Pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;
    MSG   msg;

    for (;;) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if ((LONG)(end - GetTickCount()) <= 0) break;
        Sleep(5);
    }
}

static int RunInjector(const char* injector, const char* dll)
{
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD rc = 1;
    int   n;

    n = _snprintf(cmd, sizeof(cmd) - 1, "\"%s\" --pid %lu \"%s\"",
                  injector, (unsigned long)GetCurrentProcessId(), dll);
    if (n < 0 || n >= (int)sizeof(cmd) - 1) {
        Say("inject: command line too long");
        return 0;
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));

    Say("inject: %s", cmd);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        Say("inject: CreateProcess failed err=%lu", (unsigned long)GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, 20000);
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Say("inject: exit=%lu", (unsigned long)rc);
    return rc == 0;
}

static int Topmost(HWND h)
{
    return (GetWindowLongPtrA(h, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
}

/* Re-assert TOPMOST a few times: the hook answers per request, and one
   SetWindowPos could land before the subclass is installed. */
static void ReassertTopmost(HWND h)
{
    int i;

    for (i = 0; i < 3; i++) {
        SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        Pump(150);
    }
}

static HWND MakeWindow(const char* cls, const char* title, int x)
{
    HWND h = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE, cls, title,
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             x, 120, 360, 240, NULL, NULL,
                             GetModuleHandleA(NULL), NULL);
    if (!h) return NULL;
    ShowWindow(h, SW_SHOWNOACTIVATE);
    SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(h);
    return h;
}

int main(int argc, char** argv)
{
    const char* dllPath = NULL;
    const char* injector = NULL;
    int         baseline = 0;
    int         injected = 0;
    int         rtop, ptop;
    int         ok;
    int         i;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--baseline")) {
            baseline = 1;
        } else if (!_stricmp(argv[i], "--inject") && i + 1 < argc) {
            dllPath = argv[++i];
        } else if (!injector) {
            injector = argv[i];
        }
    }

    if (!dllPath) {
        /* Default: bypass_hook32.dll next to this exe. */
        if (!baseline) {
            static char buf[MAX_PATH];
            DWORD got = GetModuleFileNameA(NULL, buf, sizeof(buf));
            if (got == 0 || got >= sizeof(buf)) {
                Say("cannot resolve own path");
                return 2;
            }
            for (i = (int)got - 1; i > 0; i--) {
                if (buf[i] == '\\') { buf[i + 1] = '\0'; break; }
            }
            strncat(buf, "bypass_hook32.dll", sizeof(buf) - strlen(buf) - 1);
            dllPath = buf;
        }
    }

    if (!RegisterOne(RCLASS) || !RegisterOne(PCLASS)) return 2;

    g_render = MakeWindow(RCLASS, "render class probe", 80);
    g_plain  = MakeWindow(PCLASS, "plain class probe", 520);
    if (!g_render || !g_plain) {
        Say("CreateWindowExA failed err=%lu", (unsigned long)GetLastError());
        return 2;
    }

    Pump(300);

    if (!baseline) {
        if (!injector) {
            static char buf[MAX_PATH];
            DWORD got = GetModuleFileNameA(NULL, buf, sizeof(buf));
            if (got == 0 || got >= sizeof(buf)) return 2;
            for (i = (int)got - 1; i > 0; i--) {
                if (buf[i] == '\\') { buf[i + 1] = '\0'; break; }
            }
            strncat(buf, "inject32.exe", sizeof(buf) - strlen(buf) - 1);
            injector = buf;
        }
        if (!RunInjector(injector, dllPath)) {
            Say("VERDICT INVALID (injector failed - no evidence either way)");
            return 2;
        }
        injected = 1;
    }

    /* Give the DLL time to subclass the windows, then make it act. */
    Pump(1200);
    ReassertTopmost(g_render);
    ReassertTopmost(g_plain);
    Pump(300);

    rtop = Topmost(g_render);
    ptop = Topmost(g_plain);

    Say("baseline=%d injected=%d", baseline, injected);
    Say("prefix  class=%s topmost=%d (want %d)", RCLASS, rtop,
        injected ? 0 : 1);
    Say("plain   class=%s topmost=%d (want 1)", PCLASS, ptop);

    if (injected) {
        ok = (rtop == 0 && ptop == 1);
    } else {
        /* Baseline: the class name alone must not change anything. */
        ok = (rtop == 1 && ptop == 1);
    }

    if (!ok) {
        Say("VERDICT FAIL (%s)",
            injected ? "prefix contract broken: tightened match, or the hook stopped stripping TOPMOST"
                     : "baseline changed without the DLL");
        return 1;
    }

    Say(injected
        ? "VERDICT PASS (MultiRender* prefix is treated as a render window; a "
          "plain class is left alone)"
        : "VERDICT PASS (baseline: no DLL, no interception)");
    return 0;
}
