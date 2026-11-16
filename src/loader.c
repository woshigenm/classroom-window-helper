/*
 * loader.c -- bypass_hook32.dll: the stable hook host.
 *
 * Owns everything that must survive a Core update:
 *   - inline hook entries + trampolines (the jmp targets never move)
 *   - IAT patching (all modules, re-scanned by the Core's loop)
 *   - logging, paths, teacher-side guard
 *   - loading / unloading bypass_hook_core.dll
 *
 * The Core (bypass_hook_core.dll) provides Hook_* implementations through
 * HrCoreApi.hooks.  Every dispatcher below is a fixed 3-line forwarder; when
 * no Core is loaded (g_table == NULL) it calls the original function, so a
 * missing/broken Core degrades to "no interception" instead of crashing.
 *
 * Adding a new interception point = add a dispatcher + real pointer + install
 * entry here (Loader rebuild, classroom client restart).  Changing a Hook_*
 * body = Core rebuild only.
 *
 * 32-bit, pure C, ASCII only.
 *
 * ⚠ This module is 32-bit ONLY: it targets i686 user32/kernel32 prologues and
 *   SetWindowLongPtrW is resolved to SetWindowLongW (LONG_PTR == LONG).  A
 *   64-bit build would need its own trampoline/dispatch code.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "bypass_abi.h"

static HMODULE g_self;
static DWORD   g_pid;
static char    g_baseDir[MAX_PATH];
static char    g_loaderPath[MAX_PATH];
static char    g_logPath[MAX_PATH];
static char    g_ownImage[64];

static HANDLE           g_logH = INVALID_HANDLE_VALUE;
static DWORD            g_logWrites;
static CRITICAL_SECTION g_logCs;
static int              g_logCsReady;

static volatile PVOID g_table;      /* const HrCoreApi* published atomically */
static HMODULE        g_coreModule;
static HrCoreApi      g_core;       /* storage for the published table      */
static HrHost         g_hostCfg;    /* Loader's ABI block: Core keeps this pointer */
static long           g_patchCount;
static int            g_scanLogged;

/* ---------------- reals (trampolines for the inline hooks) ---------------- */

static PFN_SetWindowPos        real_SetWindowPos;
static PFN_SetWindowLongW      real_SetWindowLongW;
static PFN_SetWindowLongA      real_SetWindowLongA;
static PFN_SetWindowLongPtrW   real_SetWindowLongPtrW;
static PFN_SetWindowLongPtrA   real_SetWindowLongPtrA;
static PFN_MoveWindow          real_MoveWindow;
static PFN_SetWindowPlacement  real_SetWindowPlacement;
static PFN_ShowWindow          real_ShowWindow;
static PFN_ShowWindowAsync     real_ShowWindowAsync;
static PFN_ShowCursor          real_ShowCursor;
static PFN_SetCursor           real_SetCursor;
static PFN_SwitchDesktop       real_SwitchDesktop;
static PFN_SetWindowsHookExA   real_SetWindowsHookExA;
static PFN_SetWindowsHookExW   real_SetWindowsHookExW;
static PFN_CreateWindowExW     real_CreateWindowExW;
static PFN_SetForegroundWindow real_SetForegroundWindow;
static PFN_BringWindowToTop    real_BringWindowToTop;
static PFN_SetActiveWindow     real_SetActiveWindow;
static PFN_ExitWindowsEx       real_ExitWindowsEx;

static PFN_CreateProcessA      real_CreateProcessA;
static PFN_CreateProcessW      real_CreateProcessW;
static PFN_WinExec             real_WinExec;

static PFN_BitBlt              real_BitBlt;
static PFN_CreateDCW           real_CreateDCW;
static PFN_CreateDCA           real_CreateDCA;
static PFN_GetDC               real_GetDC;
static PFN_GetWindowDC         real_GetWindowDC;

static PFN_ShellExecuteA       real_ShellExecuteA;
static PFN_ShellExecuteW       real_ShellExecuteW;

static PFN_InitiateSystemShutdownA   real_InitiateSystemShutdownA;
static PFN_InitiateSystemShutdownW   real_InitiateSystemShutdownW;
static PFN_InitiateSystemShutdownExA real_InitiateSystemShutdownExA;
static PFN_InitiateSystemShutdownExW real_InitiateSystemShutdownExW;

/* ---------------- logging ---------------- */

static void HookLog(const char* fmt, ...)
{
    char      buf[512];
    char      line[640];
    char      fmtA[512];
    va_list   ap;
    int       n;
    DWORD     wrote = 0;
    SYSTEMTIME st;

    if (!g_logCsReady) return;

    /* format string is UTF-8, %s args are ACP: normalise to ACP first */
    {
        wchar_t w[512];
        if (MultiByteToWideChar(CP_UTF8, 0, fmt, -1, w, 512) > 0 &&
            WideCharToMultiByte(CP_ACP, 0, w, -1, fmtA, sizeof(fmtA), NULL, NULL) > 0)
            fmt = fmtA;
    }

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line) - 1, "[%02u:%02u:%02u.%03u] [pid %lu] %s\r\n",
                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
                  (unsigned)st.wMilliseconds, (unsigned long)g_pid, buf);
    line[sizeof(line) - 1] = '\0';
    if (n < 0) n = (int)strlen(line);
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    EnterCriticalSection(&g_logCs);
    /* 用户删日志时, 已打开的句柄会继续写进"已删除"的文件; 每 64 行检查一次 */
    if (g_logH != INVALID_HANDLE_VALUE && (++g_logWrites & 63) == 0 &&
        GetFileAttributesA(g_logPath) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(g_logH);
        g_logH = INVALID_HANDLE_VALUE;
    }
    if (g_logH == INVALID_HANDLE_VALUE && g_logPath[0]) {
        g_logH = CreateFileA(g_logPath, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_logH != INVALID_HANDLE_VALUE)
        WriteFile(g_logH, line, (DWORD)n, &wrote, NULL);
    LeaveCriticalSection(&g_logCs);
}

/* ---------------- dispatch ---------------- */

static const HrCoreApi* Tbl(void)
{
    return (const HrCoreApi*)InterlockedCompareExchangePointer(
        (PVOID volatile*)&g_table, NULL, NULL);
}

static BOOL WINAPI L_SetWindowPos(HWND h, HWND a, int x, int y, int cx, int cy, UINT f)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetWindowPos(h, a, x, y, cx, cy, f);
    return real_SetWindowPos ? real_SetWindowPos(h, a, x, y, cx, cy, f) : FALSE;
}

static LONG WINAPI L_SetWindowLongW(HWND h, int i, LONG v)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetWindowLongW(h, i, v);
    return real_SetWindowLongW ? real_SetWindowLongW(h, i, v) : 0;
}

static LONG WINAPI L_SetWindowLongA(HWND h, int i, LONG v)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetWindowLongA(h, i, v);
    return real_SetWindowLongA ? real_SetWindowLongA(h, i, v) : 0;
}

static LONG_PTR WINAPI L_SetWindowLongPtrW(HWND h, int i, LONG_PTR v)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetWindowLongPtrW32(h, i, v);
    return real_SetWindowLongPtrW ? real_SetWindowLongPtrW(h, i, v) : 0;
}

static LONG_PTR WINAPI L_SetWindowLongPtrA(HWND h, int i, LONG_PTR v)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetWindowLongPtrA32(h, i, v);
    return real_SetWindowLongPtrA ? real_SetWindowLongPtrA(h, i, v) : 0;
}

static BOOL WINAPI L_MoveWindow(HWND h, int x, int y, int cx, int cy, BOOL r)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.MoveWindow(h, x, y, cx, cy, r);
    return real_MoveWindow ? real_MoveWindow(h, x, y, cx, cy, r) : FALSE;
}

static BOOL WINAPI L_SetWindowPlacement(HWND h, const WINDOWPLACEMENT* wp)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetWindowPlacement(h, wp);
    return real_SetWindowPlacement ? real_SetWindowPlacement(h, wp) : FALSE;
}

static BOOL WINAPI L_ShowWindow(HWND h, int c)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.ShowWindow(h, c);
    return real_ShowWindow ? real_ShowWindow(h, c) : FALSE;
}

static BOOL WINAPI L_ShowWindowAsync(HWND h, int c)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.ShowWindowAsync(h, c);
    return real_ShowWindowAsync ? real_ShowWindowAsync(h, c) : FALSE;
}

static int WINAPI L_ShowCursor(BOOL b)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.ShowCursor(b);
    return real_ShowCursor ? real_ShowCursor(b) : 0;
}

static HCURSOR WINAPI L_SetCursor(HCURSOR c)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetCursor(c);
    return real_SetCursor ? real_SetCursor(c) : NULL;
}

static BOOL WINAPI L_SwitchDesktop(HDESK d)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SwitchDesktop(d);
    return real_SwitchDesktop ? real_SwitchDesktop(d) : FALSE;
}

static HHOOK WINAPI L_SetWindowsHookExA(int id, HOOKPROC p, HINSTANCE m, DWORD t)
{
    const HrCoreApi* h = Tbl();
    if (h) return h->hooks.SetWindowsHookExA(id, p, m, t);
    return real_SetWindowsHookExA ? real_SetWindowsHookExA(id, p, m, t) : NULL;
}

static HHOOK WINAPI L_SetWindowsHookExW(int id, HOOKPROC p, HINSTANCE m, DWORD t)
{
    const HrCoreApi* h = Tbl();
    if (h) return h->hooks.SetWindowsHookExW(id, p, m, t);
    return real_SetWindowsHookExW ? real_SetWindowsHookExW(id, p, m, t) : NULL;
}

static HWND WINAPI L_CreateWindowExW(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD st,
                                     int x, int y, int w, int h, HWND parent,
                                     HMENU menu, HINSTANCE inst, LPVOID param)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.CreateWindowExW(ex, cls, name, st, x, y, w, h,
                                           parent, menu, inst, param);
    return real_CreateWindowExW ? real_CreateWindowExW(ex, cls, name, st, x, y, w, h,
                                                       parent, menu, inst, param) : NULL;
}

static BOOL WINAPI L_SetForegroundWindow(HWND h)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetForegroundWindow(h);
    return real_SetForegroundWindow ? real_SetForegroundWindow(h) : FALSE;
}

static BOOL WINAPI L_BringWindowToTop(HWND h)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.BringWindowToTop(h);
    return real_BringWindowToTop ? real_BringWindowToTop(h) : FALSE;
}

static HWND WINAPI L_SetActiveWindow(HWND h)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.SetActiveWindow(h);
    return real_SetActiveWindow ? real_SetActiveWindow(h) : NULL;
}

static BOOL WINAPI L_ExitWindowsEx(UINT f, DWORD r)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.ExitWindowsEx(f, r);
    return real_ExitWindowsEx ? real_ExitWindowsEx(f, r) : FALSE;
}

static BOOL WINAPI L_CreateProcessA(LPCSTR app, LPSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                    LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD fl,
                                    LPVOID env, LPCSTR cwd, LPSTARTUPINFOA si,
                                    LPPROCESS_INFORMATION pi)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.CreateProcessA(app, cmd, pa, ta, inh, fl, env, cwd, si, pi);
    return real_CreateProcessA ? real_CreateProcessA(app, cmd, pa, ta, inh, fl,
                                                     env, cwd, si, pi) : FALSE;
}

static BOOL WINAPI L_CreateProcessW(LPCWSTR app, LPWSTR cmd, LPSECURITY_ATTRIBUTES pa,
                                    LPSECURITY_ATTRIBUTES ta, BOOL inh, DWORD fl,
                                    LPVOID env, LPCWSTR cwd, LPSTARTUPINFOW si,
                                    LPPROCESS_INFORMATION pi)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.CreateProcessW(app, cmd, pa, ta, inh, fl, env, cwd, si, pi);
    return real_CreateProcessW ? real_CreateProcessW(app, cmd, pa, ta, inh, fl,
                                                     env, cwd, si, pi) : FALSE;
}

static UINT WINAPI L_WinExec(LPCSTR cmd, UINT show)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.WinExec(cmd, show);
    return real_WinExec ? real_WinExec(cmd, show) : 0;
}

static BOOL WINAPI L_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.BitBlt(d, x, y, w, h, s, sx, sy, rop);
    return real_BitBlt ? real_BitBlt(d, x, y, w, h, s, sx, sy, rop) : FALSE;
}

static HDC WINAPI L_CreateDCW(LPCWSTR drv, LPCWSTR dev, LPCWSTR port, const DEVMODEW* dm)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.CreateDCW(drv, dev, port, dm);
    return real_CreateDCW ? real_CreateDCW(drv, dev, port, dm) : NULL;
}

static HDC WINAPI L_CreateDCA(LPCSTR drv, LPCSTR dev, LPCSTR port, const DEVMODE* dm)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.CreateDCA(drv, dev, port, dm);
    return real_CreateDCA ? real_CreateDCA(drv, dev, port, dm) : NULL;
}

static HDC WINAPI L_GetDC(HWND h)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.GetDC(h);
    return real_GetDC ? real_GetDC(h) : NULL;
}

static HDC WINAPI L_GetWindowDC(HWND h)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.GetWindowDC(h);
    return real_GetWindowDC ? real_GetWindowDC(h) : NULL;
}

static HINSTANCE WINAPI L_ShellExecuteA(HWND o, LPCSTR op, LPCSTR file,
                                        LPCSTR args, LPCSTR dir, INT show)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.ShellExecuteA(o, op, file, args, dir, show);
    return real_ShellExecuteA ? real_ShellExecuteA(o, op, file, args, dir, show)
                              : (HINSTANCE)0;
}

static HINSTANCE WINAPI L_ShellExecuteW(HWND o, LPCWSTR op, LPCWSTR file,
                                        LPCWSTR args, LPCWSTR dir, INT show)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.ShellExecuteW(o, op, file, args, dir, show);
    return real_ShellExecuteW ? real_ShellExecuteW(o, op, file, args, dir, show)
                              : (HINSTANCE)0;
}

static BOOL WINAPI L_InitiateSystemShutdownA(LPSTR m, LPSTR msg, DWORD to, BOOL f, BOOL r)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.InitiateSystemShutdownA(m, msg, to, f, r);
    return real_InitiateSystemShutdownA
           ? real_InitiateSystemShutdownA(m, msg, to, f, r) : FALSE;
}

static BOOL WINAPI L_InitiateSystemShutdownW(LPWSTR m, LPWSTR msg, DWORD to, BOOL f, BOOL r)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.InitiateSystemShutdownW(m, msg, to, f, r);
    return real_InitiateSystemShutdownW
           ? real_InitiateSystemShutdownW(m, msg, to, f, r) : FALSE;
}

static BOOL WINAPI L_InitiateSystemShutdownExA(LPSTR m, LPSTR msg, DWORD to, BOOL f,
                                               BOOL r, DWORD why)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.InitiateSystemShutdownExA(m, msg, to, f, r, why);
    return real_InitiateSystemShutdownExA
           ? real_InitiateSystemShutdownExA(m, msg, to, f, r, why) : FALSE;
}

static BOOL WINAPI L_InitiateSystemShutdownExW(LPWSTR m, LPWSTR msg, DWORD to, BOOL f,
                                               BOOL r, DWORD why)
{
    const HrCoreApi* t = Tbl();
    if (t) return t->hooks.InitiateSystemShutdownExW(m, msg, to, f, r, why);
    return real_InitiateSystemShutdownExW
           ? real_InitiateSystemShutdownExW(m, msg, to, f, r, why) : FALSE;
}

/* ---------------- inline hooks ---------------- */

static void* FollowJump(void* target)
{
    unsigned char* b = (unsigned char*)target;
    int i;

    for (i = 0; i < 4; i++) {
        if (b[0] == 0xFF && b[1] == 0x25) {
            void** slot = (void**)(*(DWORD*)(b + 2));
            void*  t = slot ? *slot : NULL;
            if (!t || t == (void*)b) break;
            b = (unsigned char*)t;
            continue;
        }
        if (b[0] == 0xE9) { b = b + 5 + *(LONG*)(b + 1); continue; }
        if (b[0] == 0xEB) { b = b + 2 + (signed char)b[1]; continue; }
        break;
    }
    return b;
}

#define MAX_SUSP 64
static BOOL PatchEntry5(unsigned char* p, void* hook)
{
    int attempt;

    for (attempt = 0; attempt < 3; attempt++) {
        HANDLE snap, th[MAX_SUSP];
        THREADENTRY32 te;
        DWORD  self = GetCurrentThreadId();
        DWORD  old = 0;
        int    n = 0, i, inside = -1;
        BOOL   ok = FALSE;
        BOOL   giveUp = FALSE;      /* 有一个线程没能挂起/读不到上下文 */

        snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snap == INVALID_HANDLE_VALUE) return FALSE;
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
            do {
                if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                if (te.th32ThreadID == self) continue;
                if (n >= MAX_SUSP) { giveUp = TRUE; break; }

                th[n] = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                   THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (!th[n]) { giveUp = TRUE; break; }   /* 线程在/刚退出: 无法确认 */
                if (SuspendThread(th[n]) == (DWORD)-1) {
                    CloseHandle(th[n]);
                    giveUp = TRUE;
                    break;
                }
                n++;
            } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);

        /* ★ 挂起失败就不能写补丁: 那个线程可能正停在 [p,p+5) 里,
           写 E9 会让它执行到半个指令。放弃本轮, 放开已挂起的线程再重试。 */
        if (giveUp) {
            for (i = 0; i < n; i++) { ResumeThread(th[i]); CloseHandle(th[i]); }
            Sleep(50);
            continue;
        }

        for (i = 0; i < n; i++) {
            CONTEXT c;
            memset(&c, 0, sizeof(c));
            c.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(th[i], &c)) {
                inside = -2;        /* 读不到 EIP: 同样不能确认, 放弃本轮 */
                break;
            }
            {
                DWORD eip = (DWORD)c.Eip;
                if (eip >= (DWORD)(DWORD_PTR)p && eip < (DWORD)(DWORD_PTR)p + 5) {
                    inside = i;
                    break;
                }
            }
        }

        if (inside == -1 && VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
            *(LONG*)(p + 1) = (LONG)((unsigned char*)hook - (p + 5));
            p[0] = 0xE9;
            FlushInstructionCache(GetCurrentProcess(), p, 5);
            VirtualProtect(p, 5, old, &old);
            ok = TRUE;
        }

        for (i = 0; i < n; i++) { ResumeThread(th[i]); CloseHandle(th[i]); }
        if (ok) return TRUE;
        Sleep(50);
    }

    HookLog("[Loader] inline hook: 5-byte entry patch postponed at %p", p);
    return FALSE;
}

static BOOL InstallInlineHook(void* target, void* hook, void** realSlot)
{
    unsigned char* p;
    unsigned char* tr;
    DWORD          old;

    if (!target || !hook || !realSlot) return FALSE;
    p = (unsigned char*)FollowJump(target);
    if (p == (unsigned char*)hook) return FALSE;

    if (!((p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 &&
           p[3] == 0x8B && p[4] == 0xEC) ||
          p[0] == 0xB8 || p[0] == 0x68)) {
        unsigned char* o = (unsigned char*)target;

        if (o[0] == 0xFF && o[1] == 0x25) {
            void** slot = (void**)(*(DWORD*)(o + 2));
            void*  real = slot ? *slot : NULL;
            void** myslot;
            DWORD  old2;

            if (real) {
                myslot = (void**)VirtualAlloc(NULL, sizeof(void*),
                                              MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE);
                if (myslot) {
                    *myslot = hook;
                    if (VirtualProtect(o, 6, PAGE_EXECUTE_READWRITE, &old2)) {
                        *realSlot = real;
                        *(DWORD*)(o + 2) = (DWORD)myslot;
                        VirtualProtect(o, 6, old2, &old2);
                        FlushInstructionCache(GetCurrentProcess(), o, 6);
                        return TRUE;
                    }
                    VirtualFree(myslot, 0, MEM_RELEASE);
                }
            }
        }
        HookLog("[Loader] inline hook: unexpected prologue at %p (%02X %02X %02X %02X %02X)",
                p, p[0], p[1], p[2], p[3], p[4]);
        return FALSE;
    }

    if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC &&
        ((DWORD_PTR)p & 1) == 0) {
        unsigned char* pad = p - 5;
        BOOL padding = TRUE;
        int  i;

        for (i = 0; i < 5; i++)
            if (pad[i] != 0xCC && pad[i] != 0x90) { padding = FALSE; break; }

        if (padding && VirtualProtect(pad, 5, PAGE_EXECUTE_READWRITE, &old)) {
            *(LONG*)(pad + 1) = (LONG)((unsigned char*)hook - (pad + 5));
            pad[0] = 0xE9;
            FlushInstructionCache(GetCurrentProcess(), pad, 5);
            if (VirtualProtect(p, 2, PAGE_EXECUTE_READWRITE, &old)) {
                *realSlot = (void*)(p + 2);
                *(WORD*)p = (WORD)0xF9EB;
                FlushInstructionCache(GetCurrentProcess(), p, 2);
                return TRUE;
            }
            VirtualProtect(pad, 5, PAGE_EXECUTE_READWRITE, &old);
            pad[0] = 0xCC;
            FlushInstructionCache(GetCurrentProcess(), pad, 5);
        }
    }

    tr = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!tr) return FALSE;

    memcpy(tr, p, 5);
    tr[5] = 0xE9;
    *(LONG*)(tr + 6) = (LONG)((p + 5) - (tr + 5 + 5));
    {
        void* orig = *realSlot;    /* GetProcAddress 拿到的原函数 */

        *realSlot = tr;
        if (!PatchEntry5(p, hook)) {
            /* 入口没改成功: 回退到原函数指针, 释放 trampoline。
               (IAT 仍然指向 dispatcher, 它必须还能调到原函数。) */
            *realSlot = orig;
            VirtualFree(tr, 0, MEM_RELEASE);
            HookLog("[Loader] inline hook: IAT only for %p", p);
            return FALSE;
        }
    }
    return TRUE;
}

/* ---------------- IAT patching ---------------- */

static void PatchOne(HMODULE mod, const char* dllName, const char* fnName, void* hook)
{
    IMAGE_DOS_HEADER*        dos;
    IMAGE_NT_HEADERS*        nt;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    DWORD                    rva;

    if (!mod || mod == g_self || mod == g_coreModule) return;

    dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;

    imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + rva);
    for (; imp->Name; imp++) {
        const char*       name = (const char*)((BYTE*)mod + imp->Name);
        IMAGE_THUNK_DATA* oft;
        IMAGE_THUNK_DATA* ft;

        if (_stricmp(name, dllName) != 0) continue;
        oft = (IMAGE_THUNK_DATA*)((BYTE*)mod +
              (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        ft  = (IMAGE_THUNK_DATA*)((BYTE*)mod + imp->FirstThunk);

        for (; oft->u1.Function; oft++, ft++) {
            IMAGE_IMPORT_BY_NAME* ibn;
            DWORD old;

            if (oft->u1.Ordinal & IMAGE_ORDINAL_FLAG) continue;
            ibn = (IMAGE_IMPORT_BY_NAME*)((BYTE*)mod + oft->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, fnName) != 0) continue;
            if (ft->u1.Function == (DWORD_PTR)hook) continue;

            if (VirtualProtect(&ft->u1.Function, sizeof(DWORD_PTR),
                               PAGE_READWRITE, &old)) {
                ft->u1.Function = (DWORD_PTR)hook;
                VirtualProtect(&ft->u1.Function, sizeof(DWORD_PTR), old, &old);
                g_patchCount++;
            }
        }
    }
}

static long PatchAllModules(void)
{
    HMODULE mods[512];
    DWORD   needed = 0, i;
    DWORD   count = 0;
    long    before = g_patchCount;
    long    delta;

    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return 0;
    if (needed > sizeof(mods)) needed = sizeof(mods);
    count = needed / sizeof(HMODULE);

    for (i = 0; i < needed / sizeof(HMODULE); i++) {
        PatchOne(mods[i], "user32.dll", "SetWindowPos",        (void*)L_SetWindowPos);
        PatchOne(mods[i], "user32.dll", "SetWindowLongW",      (void*)L_SetWindowLongW);
        PatchOne(mods[i], "user32.dll", "SetWindowLongA",      (void*)L_SetWindowLongA);
        PatchOne(mods[i], "user32.dll", "SetWindowLongPtrW",   (void*)L_SetWindowLongPtrW);
        PatchOne(mods[i], "user32.dll", "SetWindowLongPtrA",   (void*)L_SetWindowLongPtrA);
        PatchOne(mods[i], "user32.dll", "MoveWindow",          (void*)L_MoveWindow);
        PatchOne(mods[i], "user32.dll", "SetWindowPlacement",  (void*)L_SetWindowPlacement);
        PatchOne(mods[i], "user32.dll", "ShowWindow",          (void*)L_ShowWindow);
        PatchOne(mods[i], "user32.dll", "ShowWindowAsync",     (void*)L_ShowWindowAsync);
        PatchOne(mods[i], "user32.dll", "ShowCursor",          (void*)L_ShowCursor);
        PatchOne(mods[i], "user32.dll", "SetCursor",           (void*)L_SetCursor);
        PatchOne(mods[i], "user32.dll", "SwitchDesktop",       (void*)L_SwitchDesktop);
        PatchOne(mods[i], "user32.dll", "SetWindowsHookExA",   (void*)L_SetWindowsHookExA);
        PatchOne(mods[i], "user32.dll", "SetWindowsHookExW",   (void*)L_SetWindowsHookExW);
        PatchOne(mods[i], "kernel32.dll", "CreateProcessA",    (void*)L_CreateProcessA);
        PatchOne(mods[i], "kernel32.dll", "CreateProcessW",    (void*)L_CreateProcessW);
        PatchOne(mods[i], "user32.dll",   "ExitWindowsEx",     (void*)L_ExitWindowsEx);
        PatchOne(mods[i], "kernel32.dll", "WinExec",           (void*)L_WinExec);
        PatchOne(mods[i], "shell32.dll",  "ShellExecuteA",     (void*)L_ShellExecuteA);
        PatchOne(mods[i], "shell32.dll",  "ShellExecuteW",     (void*)L_ShellExecuteW);
        PatchOne(mods[i], "advapi32.dll", "InitiateSystemShutdownA",
                 (void*)L_InitiateSystemShutdownA);
        PatchOne(mods[i], "advapi32.dll", "InitiateSystemShutdownW",
                 (void*)L_InitiateSystemShutdownW);
        PatchOne(mods[i], "advapi32.dll", "InitiateSystemShutdownExA",
                 (void*)L_InitiateSystemShutdownExA);
        PatchOne(mods[i], "advapi32.dll", "InitiateSystemShutdownExW",
                 (void*)L_InitiateSystemShutdownExW);
        PatchOne(mods[i], "user32.dll", "CreateWindowExW",     (void*)L_CreateWindowExW);
        PatchOne(mods[i], "user32.dll", "SetForegroundWindow", (void*)L_SetForegroundWindow);
        PatchOne(mods[i], "user32.dll", "BringWindowToTop",    (void*)L_BringWindowToTop);
        PatchOne(mods[i], "user32.dll", "SetActiveWindow",     (void*)L_SetActiveWindow);
        PatchOne(mods[i], "gdi32.dll",   "BitBlt",             (void*)L_BitBlt);
        PatchOne(mods[i], "gdi32.dll",   "CreateDCW",          (void*)L_CreateDCW);
        PatchOne(mods[i], "gdi32.dll",   "CreateDCA",          (void*)L_CreateDCA);
        PatchOne(mods[i], "user32.dll",  "GetDC",              (void*)L_GetDC);
        PatchOne(mods[i], "user32.dll",  "GetWindowDC",        (void*)L_GetWindowDC);
    }

    delta = g_patchCount - before;
    /* 第一次扫描（Core 加载前）和后续"有新模块被加载"时各记一行:
       既能确认扫到了多少模块, 也能看出 Core 的增量是不是真的 0。 */
    if (!g_scanLogged || delta > 0) {
        g_scanLogged = 1;
        HookLog("[Loader] IAT scan: %lu modules, %ld slots patched",
                (unsigned long)count, delta);
    }
    return delta;
}

/* ---------------- paths / teacher guard ---------------- */

static void BaseNameOf(const char* path, char* out, int n)
{
    const char* p;
    if (!n) return;
    p = strrchr(path, '\\');
    p = p ? p + 1 : path;
    _snprintf(out, (size_t)n - 1, "%s", p);
    out[n - 1] = '\0';
}

static BOOL IsTeacherSideImage(const char* img)
{
    if (!img || !img[0]) return FALSE;
    return _stricmp(img, "Teacher.exe") == 0 ||
           _stricmp(img, "MultiTeach.exe") == 0 ||
           _stricmp(img, "ScreenSender.exe") == 0 ||
           _stricmp(img, "MediaFileSender.exe") == 0 ||
           _stricmp(img, "VideoTechConsole.exe") == 0;
}

static void BuildPaths(void)
{
    char dll[MAX_PATH];
    char* p;

    g_baseDir[0] = '\0';
    g_loaderPath[0] = '\0';
    g_logPath[0] = '\0';

    if (!GetModuleFileNameA(g_self, dll, sizeof(dll))) return;
    _snprintf(g_loaderPath, sizeof(g_loaderPath) - 1, "%s", dll);
    g_loaderPath[sizeof(g_loaderPath) - 1] = '\0';

    p = strrchr(dll, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(g_baseDir, sizeof(g_baseDir) - 1, "%s", dll);
    g_baseDir[sizeof(g_baseDir) - 1] = '\0';
    _snprintf(g_logPath, sizeof(g_logPath) - 1, "%sbypass_hook.log", dll);
    g_logPath[sizeof(g_logPath) - 1] = '\0';
}

/* ---------------- setup ---------------- */

static void ResolveReals(void)
{
    HMODULE u32 = GetModuleHandleA("user32.dll");
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    HMODULE g32 = GetModuleHandleA("gdi32.dll");
    HMODULE s32 = GetModuleHandleA("shell32.dll");
    HMODULE a32 = GetModuleHandleA("advapi32.dll");

    if (!u32) u32 = LoadLibraryA("user32.dll");
    if (!k32) k32 = LoadLibraryA("kernel32.dll");
    if (!g32) g32 = LoadLibraryA("gdi32.dll");

    if (u32) {
        real_SetWindowPos        = (PFN_SetWindowPos)(void*)GetProcAddress(u32, "SetWindowPos");
        real_SetWindowLongW      = (PFN_SetWindowLongW)(void*)GetProcAddress(u32, "SetWindowLongW");
        real_SetWindowLongA      = (PFN_SetWindowLongA)(void*)GetProcAddress(u32, "SetWindowLongA");
        real_SetWindowLongPtrW   = (PFN_SetWindowLongPtrW)(void*)GetProcAddress(u32, "SetWindowLongPtrW");
        real_SetWindowLongPtrA   = (PFN_SetWindowLongPtrA)(void*)GetProcAddress(u32, "SetWindowLongPtrA");
        real_MoveWindow          = (PFN_MoveWindow)(void*)GetProcAddress(u32, "MoveWindow");
        real_SetWindowPlacement  = (PFN_SetWindowPlacement)(void*)GetProcAddress(u32, "SetWindowPlacement");
        real_ShowWindow          = (PFN_ShowWindow)(void*)GetProcAddress(u32, "ShowWindow");
        real_ShowWindowAsync     = (PFN_ShowWindowAsync)(void*)GetProcAddress(u32, "ShowWindowAsync");
        real_ShowCursor          = (PFN_ShowCursor)(void*)GetProcAddress(u32, "ShowCursor");
        real_SetCursor           = (PFN_SetCursor)(void*)GetProcAddress(u32, "SetCursor");
        real_SwitchDesktop       = (PFN_SwitchDesktop)(void*)GetProcAddress(u32, "SwitchDesktop");
        real_SetWindowsHookExA   = (PFN_SetWindowsHookExA)(void*)GetProcAddress(u32, "SetWindowsHookExA");
        real_SetWindowsHookExW   = (PFN_SetWindowsHookExW)(void*)GetProcAddress(u32, "SetWindowsHookExW");
        real_CreateWindowExW     = (PFN_CreateWindowExW)(void*)GetProcAddress(u32, "CreateWindowExW");
        real_SetForegroundWindow = (PFN_SetForegroundWindow)(void*)GetProcAddress(u32, "SetForegroundWindow");
        real_BringWindowToTop    = (PFN_BringWindowToTop)(void*)GetProcAddress(u32, "BringWindowToTop");
        real_SetActiveWindow     = (PFN_SetActiveWindow)(void*)GetProcAddress(u32, "SetActiveWindow");
        real_ExitWindowsEx       = (PFN_ExitWindowsEx)(void*)GetProcAddress(u32, "ExitWindowsEx");
        real_GetDC               = (PFN_GetDC)(void*)GetProcAddress(u32, "GetDC");
        real_GetWindowDC         = (PFN_GetWindowDC)(void*)GetProcAddress(u32, "GetWindowDC");
    }
    if (k32) {
        real_CreateProcessA = (PFN_CreateProcessA)(void*)GetProcAddress(k32, "CreateProcessA");
        real_CreateProcessW = (PFN_CreateProcessW)(void*)GetProcAddress(k32, "CreateProcessW");
        real_WinExec        = (PFN_WinExec)(void*)GetProcAddress(k32, "WinExec");
    }
    if (g32) {
        real_BitBlt   = (PFN_BitBlt)(void*)GetProcAddress(g32, "BitBlt");
        real_CreateDCW = (PFN_CreateDCW)(void*)GetProcAddress(g32, "CreateDCW");
        real_CreateDCA = (PFN_CreateDCA)(void*)GetProcAddress(g32, "CreateDCA");
    }
    if (s32) {
        real_ShellExecuteA = (PFN_ShellExecuteA)(void*)GetProcAddress(s32, "ShellExecuteA");
        real_ShellExecuteW = (PFN_ShellExecuteW)(void*)GetProcAddress(s32, "ShellExecuteW");
    }
    if (a32) {
        real_InitiateSystemShutdownA   = (PFN_InitiateSystemShutdownA)(void*)GetProcAddress(a32, "InitiateSystemShutdownA");
        real_InitiateSystemShutdownW   = (PFN_InitiateSystemShutdownW)(void*)GetProcAddress(a32, "InitiateSystemShutdownW");
        real_InitiateSystemShutdownExA = (PFN_InitiateSystemShutdownExA)(void*)GetProcAddress(a32, "InitiateSystemShutdownExA");
        real_InitiateSystemShutdownExW = (PFN_InitiateSystemShutdownExW)(void*)GetProcAddress(a32, "InitiateSystemShutdownExW");
    }

    /* 32-bit user32 does not export the Ptr variants */
    if (!real_SetWindowLongPtrW) real_SetWindowLongPtrW = (PFN_SetWindowLongPtrW)(void*)real_SetWindowLongW;
    if (!real_SetWindowLongPtrA) real_SetWindowLongPtrA = (PFN_SetWindowLongPtrA)(void*)real_SetWindowLongA;
}

/* Install the 8 inline hooks.  These are the entries that must never point
   into the Core: they live here for the lifetime of the process. */
static int InstallInlineHooks(void)
{
    int ok = 0, n = 0;

    if (real_ShowWindow) {
        n++; if (InstallInlineHook((void*)real_ShowWindow, (void*)L_ShowWindow,
                                   (void**)&real_ShowWindow)) ok++;
    }
    if (real_ShowWindowAsync) {
        n++; if (InstallInlineHook((void*)real_ShowWindowAsync, (void*)L_ShowWindowAsync,
                                   (void**)&real_ShowWindowAsync)) ok++;
    }
    if (real_WinExec) {
        n++; if (InstallInlineHook((void*)real_WinExec, (void*)L_WinExec,
                                   (void**)&real_WinExec)) ok++;
    }
    if (real_CreateProcessW) {
        n++; if (InstallInlineHook((void*)real_CreateProcessW, (void*)L_CreateProcessW,
                                   (void**)&real_CreateProcessW)) ok++;
    }
    if (real_CreateProcessA) {
        n++; if (InstallInlineHook((void*)real_CreateProcessA, (void*)L_CreateProcessA,
                                   (void**)&real_CreateProcessA)) ok++;
    }
    if (real_ExitWindowsEx) {
        n++; if (InstallInlineHook((void*)real_ExitWindowsEx, (void*)L_ExitWindowsEx,
                                   (void**)&real_ExitWindowsEx)) ok++;
    }
    if (real_ShellExecuteA) {
        n++; if (InstallInlineHook((void*)real_ShellExecuteA, (void*)L_ShellExecuteA,
                                   (void**)&real_ShellExecuteA)) ok++;
    }
    if (real_ShellExecuteW) {
        n++; if (InstallInlineHook((void*)real_ShellExecuteW, (void*)L_ShellExecuteW,
                                   (void**)&real_ShellExecuteW)) ok++;
    }
    return (n == ok) ? n : -ok;
}

static void FillHost(HrHost* host)
{
    memset(host, 0, sizeof(*host));
    host->abi_version = HR_ABI_VERSION;
    host->size        = sizeof(*host);
    host->base_dir    = g_baseDir;
    host->loader_path = g_loaderPath;
    host->log         = HookLog;
    host->patch_modules = PatchAllModules;

    host->real.SetWindowPos        = (void*)real_SetWindowPos;
    host->real.SetWindowLongW      = (void*)real_SetWindowLongW;
    host->real.SetWindowLongA      = (void*)real_SetWindowLongA;
    host->real.SetWindowLongPtrW32   = (void*)real_SetWindowLongPtrW;
    host->real.SetWindowLongPtrA32   = (void*)real_SetWindowLongPtrA;
    host->real.MoveWindow          = (void*)real_MoveWindow;
    host->real.SetWindowPlacement  = (void*)real_SetWindowPlacement;
    host->real.ShowWindow          = (void*)real_ShowWindow;
    host->real.ShowWindowAsync     = (void*)real_ShowWindowAsync;
    host->real.ShowCursor          = (void*)real_ShowCursor;
    host->real.SetCursor           = (void*)real_SetCursor;
    host->real.SwitchDesktop       = (void*)real_SwitchDesktop;
    host->real.SetWindowsHookExA   = (void*)real_SetWindowsHookExA;
    host->real.SetWindowsHookExW   = (void*)real_SetWindowsHookExW;
    host->real.CreateWindowExW     = (void*)real_CreateWindowExW;
    host->real.SetForegroundWindow = (void*)real_SetForegroundWindow;
    host->real.BringWindowToTop    = (void*)real_BringWindowToTop;
    host->real.SetActiveWindow     = (void*)real_SetActiveWindow;
    host->real.ExitWindowsEx       = (void*)real_ExitWindowsEx;

    host->real.CreateProcessA      = (void*)real_CreateProcessA;
    host->real.CreateProcessW      = (void*)real_CreateProcessW;
    host->real.WinExec             = (void*)real_WinExec;

    host->real.BitBlt              = (void*)real_BitBlt;
    host->real.CreateDCW           = (void*)real_CreateDCW;
    host->real.CreateDCA           = (void*)real_CreateDCA;
    host->real.GetDC               = (void*)real_GetDC;
    host->real.GetWindowDC         = (void*)real_GetWindowDC;

    host->real.ShellExecuteA       = (void*)real_ShellExecuteA;
    host->real.ShellExecuteW       = (void*)real_ShellExecuteW;

    host->real.InitiateSystemShutdownA   = (void*)real_InitiateSystemShutdownA;
    host->real.InitiateSystemShutdownW   = (void*)real_InitiateSystemShutdownW;
    host->real.InitiateSystemShutdownExA = (void*)real_InitiateSystemShutdownExA;
    host->real.InitiateSystemShutdownExW = (void*)real_InitiateSystemShutdownExW;
}

/* 33 个入口必须全部非 NULL: dispatcher 不做逐个判空 (热路径)。
   缺一个就拒绝发布, 否则那一条拦截一触发就是跳 0。 */
static BOOL HooksComplete(const HrHooks* h)
{
    return h->SetWindowPos && h->SetWindowLongW && h->SetWindowLongA &&
           h->SetWindowLongPtrW32 && h->SetWindowLongPtrA32 &&
           h->MoveWindow && h->SetWindowPlacement && h->ShowWindow &&
           h->ShowWindowAsync && h->ShowCursor && h->SetCursor &&
           h->SwitchDesktop && h->SetWindowsHookExA && h->SetWindowsHookExW &&
           h->CreateWindowExW && h->SetForegroundWindow &&
           h->BringWindowToTop && h->SetActiveWindow && h->ExitWindowsEx &&
           h->CreateProcessA && h->CreateProcessW && h->WinExec &&
           h->BitBlt && h->CreateDCW && h->CreateDCA && h->GetDC &&
           h->GetWindowDC && h->ShellExecuteA && h->ShellExecuteW &&
           h->InitiateSystemShutdownA && h->InitiateSystemShutdownW &&
           h->InitiateSystemShutdownExA && h->InitiateSystemShutdownExW;
}

static BOOL LoadCore(void)
{
    char    path[MAX_PATH];
    PFN_Core_Init init;

    _snprintf(path, sizeof(path) - 1, "%s" HR_CORE_NAME, g_baseDir);
    path[sizeof(path) - 1] = '\0';

    /* Core 可能有同目录依赖 (P2 起还能从缓存目录加载) */
    g_coreModule = LoadLibraryExA(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_coreModule) {
        HookLog("[Loader] CORE LOAD FAILED: %s err=%lu (hooks passthrough)",
                path, (unsigned long)GetLastError());
        return FALSE;
    }
    init = (PFN_Core_Init)(void*)GetProcAddress(g_coreModule, HR_CORE_INIT);
    if (!init) {
        HookLog("[Loader] %s has no %s (hooks passthrough)", path, HR_CORE_INIT);
        FreeLibrary(g_coreModule);
        g_coreModule = NULL;
        return FALSE;
    }

    /* ★ 必须是静态存储: Core 会把 g_host 一直留着用, 不能指向 Manager 的栈 */
    FillHost(&g_hostCfg);
    memset(&g_core, 0, sizeof(g_core));

    if (!init(&g_hostCfg, &g_core)) {
        /* Core 自己检查了 ABI, 失败时什么都没做 -> FreeLibrary 安全 */
        HookLog("[Loader] Core_Init failed (returned 0) - FreeLibrary safe");
        FreeLibrary(g_coreModule);
        g_coreModule = NULL;
        return FALSE;
    }

    /* 走到这里 Core 已经初始化 (线程已起), **绝不能 FreeLibrary**:
       返回的 ABI/表不对时只能不发布 (钩子保持直通), 让 Core 留在内存里。 */
    if (g_core.abi_version != HR_ABI_VERSION || g_core.size < sizeof(HrCoreApi)) {
        HookLog("[Loader] Core_Init returned bad ABI (abi=%u size=%u) - "
                "leaving it mapped, hooks passthrough",
                g_core.abi_version, g_core.size);
        return FALSE;
    }
    if (!HooksComplete(&g_core.hooks)) {
        HookLog("[Loader] Core hook table incomplete - leaving it mapped, "
                "hooks passthrough");
        return FALSE;
    }

    InterlockedExchangePointer((PVOID volatile*)&g_table, (PVOID)&g_core);
    HookLog("[Loader] core loaded: %s build=%s", path,
            g_core.build_id ? g_core.build_id : "?");
    return TRUE;
}

static DWORD WINAPI Manager(LPVOID p)
{
    char full[MAX_PATH];
    int  inlineOk;

    (void)p;

    Sleep(200);
    BuildPaths();

    GetModuleFileNameA(NULL, full, sizeof(full));
    BaseNameOf(full, g_ownImage, sizeof(g_ownImage));

    HookLog("[build] loader=%s built=%s %s pid=%lu",
            HR_LOADER_NAME, __DATE__, __TIME__, (unsigned long)g_pid);

    if (IsTeacherSideImage(g_ownImage)) {
        HookLog("[Loader] skip: %s is teacher-side (no hooks, no core)", g_ownImage);
        return 0;
    }

    ResolveReals();
    inlineOk = InstallInlineHooks();
    HookLog("[Loader] inline hooks: %s", inlineOk < 0 ? "PARTIAL" : "all installed");

    PatchAllModules();

    LoadCore();
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        HANDLE th;

        g_self = hInst;
        g_pid  = GetCurrentProcessId();
        DisableThreadLibraryCalls(hInst);

        InitializeCriticalSection(&g_logCs);
        g_logCsReady = 1;

        th = CreateThread(NULL, 0, Manager, NULL, 0, NULL);
        if (th) CloseHandle(th);
    }
    return TRUE;
}
