/* ============================================================
 *  wmode_test.c -- 控制端「窗口模式广播」小窗测试载体
 *
 *  模拟学生机上收到的窗口模式广播:
 *    - 渲染窗出生就是小尺寸 + WS_EX_TOPMOST (不经过我们的降级)
 *    - 应用每 500ms 重申一次 HWND_TOPMOST (与渲染器行为一致)
 *    - 中间夹一次 SetForegroundWindow(参考窗): 模拟"学生点了自己的窗口"
 *
 *  探针 (每轮 500ms 记录一次):
 *    top   = 渲染窗还有没有 WS_EX_TOPMOST
 *    above = 渲染窗是不是排在参考窗之上 (Z 序更靠前)
 *
 *  第二阶段 (仅注入时): 用户把小窗拖到接近满屏 -> 松手 -> 应用把出生时缓存的
 *  小矩形再发三遍 (用 GetProcAddress 动态调 SetWindowPos, 和真渲染器一样绕开
 *  IAT, 只考窗口子类那层判断)。期望窗口守住用户的尺寸 (resize_kept=1);
 *  旧逻辑会把"用户刚动过窗口"直接当成"用户还原"放行, 窗口当场缩回右下角。
 *
 *  预期基线 (不注入):              top=1 above=1
 *  旧版 DLL (TOPMOST->NOTOPMOST):  top=0 above=1   <- 缺陷: 每次重申都抬到
 *                                                 参考窗之上
 *  新版 DLL (SWP_NOZORDER):        top=0 above=0   <- 修复: Z 序被冻结
 *
 *  用法:
 *    wmode_test.exe [--ticks N] [--inject <bypass_hook32.dll>] [<inject32.exe>]
 *
 *  注意: 窗口类名必须以渲染器白名单前缀开头 (MultiRender...), 只有这样
 *        bypass_hook.c 的 IsRenderWindowClass() 才认它是广播窗口。
 *  本文件刻意只用 ANSI (RegisterClassA + DefWindowProcA) 的配套组合。
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define RENDER_CLASS "MultiRenderWModeTest"
#define REF_CLASS    "WModeRefTest"

#define WIN_X 120
#define WIN_Y 120
#define WIN_W 640
#define WIN_H 360

static HWND   g_win;
static HWND   g_ref;
static int    g_polls;
static int    g_topSet;
static int    g_aboveN;
static int    g_resizeKept = 1;   /* Phase 2: 用户拖大后应用想改回去, 窗口守住了吗 */
static FILE*  g_logFile;

static void Say(const char* fmt, ...)
{
    char buf[512];
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    puts(buf);
    fflush(stdout);
    if (g_logFile) {
        fputs(buf, g_logFile);
        fputc('\n', g_logFile);
        fflush(g_logFile);
    }
}

static LRESULT CALLBACK RenderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(h, msg, wp, lp);
}

static LRESULT CALLBACK RefProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(h, msg, wp, lp);
}

static BOOL RegisterOne(const char* name, WNDPROC proc)
{
    WNDCLASSA wc;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = proc;
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

/* 顶层窗口 Z 序里的位置: 0 = 最前 (topmost 带最先枚举) */
static int ZIndex(HWND target)
{
    HWND h;
    int  i = 0;

    if (!target) return -1;
    for (h = GetTopWindow(NULL); h; h = GetWindow(h, GW_HWNDNEXT), i++)
        if (h == target) return i;
    return -1;
}

/* 1 = a 在 b 之上, 0 = 不在, -1 = 枚举失败 */
static int IsAbove(HWND a, HWND b)
{
    int ia = ZIndex(a);
    int ib = ZIndex(b);

    if (ia < 0 || ib < 0) return -1;
    return (ia < ib) ? 1 : 0;
}

/* 边跑消息边等 —— 注入的 DLL 是靠 PostMessage 把修复动作丢回窗口线程的,
   必须真的泵消息才能看到效果 (Sleep 只会把消息压在队列里)。 */
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

    _snprintf(cmd, sizeof(cmd) - 1,
              "\"%s\" --pid %lu \"%s\"",
              injector, (unsigned long)GetCurrentProcessId(), dll);
    cmd[sizeof(cmd) - 1] = '\0';

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

int main(int argc, char** argv)
{
    int         ticks = 25;
    const char* dllPath = NULL;
    const char* injector = NULL;
    char        logPath[MAX_PATH] = "";
    int         i, ok;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--ticks") && i + 1 < argc) {
            ticks = atoi(argv[++i]);
        } else if (!_stricmp(argv[i], "--inject") && i + 1 < argc) {
            dllPath = argv[++i];
        } else if (!injector) {
            injector = argv[i];
        }
    }

    /* 日志写在 exe 旁边, 便于注入后仍然留证据 */
    {
        char exe[MAX_PATH] = "";
        char* p;
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        p = strrchr(exe, '\\');
        if (p) *(p + 1) = '\0';
        _snprintf(logPath, sizeof(logPath) - 1, "%swmode_test.log", exe);
        logPath[sizeof(logPath) - 1] = '\0';
        g_logFile = fopen(logPath, "w");
    }

    SetProcessDPIAware();

    Say("wmode_test pid=%lu ticks=%d dll=%s",
        (unsigned long)GetCurrentProcessId(), ticks, dllPath ? dllPath : "(none)");
    Say("screen=%dx%d",
        (int)GetSystemMetrics(SM_CXSCREEN), (int)GetSystemMetrics(SM_CYSCREEN));

    SetLastError(0);
    if (!RegisterOne(RENDER_CLASS, RenderProc)) return 2;
    if (!RegisterOne(REF_CLASS, RefProc))       return 2;
    Say("classes registered: %s / %s", RENDER_CLASS, REF_CLASS);

    /* 参考窗: 一个普通(非置顶)顶层窗口, 代表"学生正在用的窗口" */
    g_ref = CreateWindowExA(WS_EX_NOACTIVATE, REF_CLASS, "reference (student app)",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            760, 420, 480, 300,
                            NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!g_ref) {
        Say("CreateWindowExA(ref) failed err=%lu", (unsigned long)GetLastError());
        return 3;
    }
    ShowWindow(g_ref, SW_SHOWNOACTIVATE);

    /* 渲染窗: 出生即小尺寸 + WS_EX_TOPMOST (窗口模式广播的真实状态) */
    g_win = CreateWindowExA(WS_EX_TOPMOST | WS_EX_NOACTIVATE, RENDER_CLASS,
                            "window-mode broadcast (test)",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            WIN_X, WIN_Y, WIN_W, WIN_H,
                            NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!g_win) {
        Say("CreateWindowExA(render) failed err=%lu", (unsigned long)GetLastError());
        return 3;
    }
    Say("hwnd win=%p ref=%p", (void*)g_win, (void*)g_ref);
    Say("win exstyle=0x%08X style=0x%08X",
        (unsigned)GetWindowLongPtrA(g_win, GWL_EXSTYLE),
        (unsigned)GetWindowLongPtrA(g_win, GWL_STYLE));

    ShowWindow(g_win, SW_SHOWNOACTIVATE);
    /* explicit foreground grab stays: it simulates "the student clicked his
       own window", and WS_EX_NOACTIVATE does not block SetForegroundWindow */
    SetForegroundWindow(g_win);
    Pump(300);

    if (dllPath) {
        if (!injector) {
            /* 默认取 exe 同目录的 inject32.exe */
            static char buf[MAX_PATH];
            char exe[MAX_PATH] = "";
            char* p;
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            p = strrchr(exe, '\\');
            if (p) *(p + 1) = '\0';
            _snprintf(buf, sizeof(buf) - 1, "%sinject32.exe", exe);
            injector = buf;
        }
        if (!RunInjector(injector, dllPath))
            Say("WARNING: injector failed, continuing as baseline");
        /* DLL 里 Worker 有 200ms 的建窗等待 + 打补丁时间 */
        Pump(1500);

        /* 真机上这个属性由 DLL 在"接管/降级"窗口时写入 (MarkManaged)。
           窗口模式广播窗是"出生就小", 不会走降级路径, 所以测试里自己补上,
           否则子类里"几何归用户"那一整段根本不会执行。 */
        SetPropA(g_win, "BypassSmall", (HANDLE)1);
        Say("marked managed (BypassSmall) so the geometry rules apply");
    }

    for (i = 1; i <= ticks; i++) {
        int top, above;

        Pump(500);

        /* 学生点自己的窗口 */
        SetForegroundWindow(g_ref);
        Pump(120);

        /* 渲染器 500ms 定时器: 重申 HWND_TOPMOST (只改 Z 序, 不改几何) */
        SetWindowPos(g_win, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE);
        Pump(160);

        top   = (GetWindowLongPtrA(g_win, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1 : 0;
        above = IsAbove(g_win, g_ref);

        g_polls++;
        if (top)   g_topSet++;
        if (above == 1) g_aboveN++;

        Say("poll%04d top=%d above=%d fg=%p idxW=%d idxR=%d",
            i, top, above,
            (void*)GetForegroundWindow(),
            ZIndex(g_win), ZIndex(g_ref));
    }

    /* ---- Phase 2: 用户把小窗拖到接近满屏, 应用又把缓存的小矩形发回来 ----
       19:18 的现场日志里, 渲染器是**动态取地址**调 SetWindowPos 的 (不走 IAT),
       所以只有窗口子类那一层能拦。这里同样用 GetProcAddress 拿函数指针,
       绕开我们自己打的 IAT 补丁, 真实地考一遍子类里的"几何归用户"判断。

       模拟的用户动作: 写一次 BypassUserCmd 时间戳 (等价于 SC_SIZE/SC_MOVE 里
       记录的那一下 —— 不直接发 SC_SIZE, 那会进系统的模态拖拽循环把测试卡住),
       再走一遍 WM_ENTERSIZEMOVE → 改大小 → WM_EXITSIZEMOVE (松手)。 */
    if (dllPath && g_win && IsWindow(g_win)) {
        typedef BOOL (WINAPI *PFN_SWP)(HWND, HWND, int, int, int, int, UINT);
        PFN_SWP dynSwp = (PFN_SWP)(void*)GetProcAddress(GetModuleHandleA("user32.dll"),
                                                        "SetWindowPos");
        RECT        small, now;
        MONITORINFO mi;
        HMONITOR    mon;
        int uw = 0, uh = 0, ux = 0, uy = 0;

        GetWindowRect(g_win, &small);
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        mon = MonitorFromWindow(g_win, MONITOR_DEFAULTTONEAREST);

        if (dynSwp && GetMonitorInfoA(mon, &mi)) {
            uw = (mi.rcWork.right - mi.rcWork.left) - 16;   /* 几乎铺满, 但不是整屏 */
            uh = (mi.rcWork.bottom - mi.rcWork.top) - 16;
            ux = mi.rcWork.left + 8;
            uy = mi.rcWork.top + 8;

            Say("resize: user drags %dx%d@(%d,%d) -> %dx%d@(%d,%d)",
                (int)(small.right - small.left), (int)(small.bottom - small.top),
                (int)small.left, (int)small.top, uw, uh, ux, uy);

            SetPropA(g_win, "BypassUserCmd", (HANDLE)(ULONG_PTR)GetTickCount());
            SendMessageA(g_win, WM_ENTERSIZEMOVE, 0, 0);
            SetWindowPos(g_win, NULL, ux, uy, uw, uh, SWP_NOZORDER | SWP_NOACTIVATE);
            Pump(80);
            SendMessageA(g_win, WM_EXITSIZEMOVE, 0, 0);
            Pump(80);

            /* 渲染器 500ms 定时器: 把出生时缓存的小矩形再发一遍 (它想要回去) */
            for (i = 0; i < 3; i++) {
                dynSwp(g_win, NULL, (int)small.left, (int)small.top,
                       (int)(small.right - small.left),
                       (int)(small.bottom - small.top),
                       SWP_NOZORDER | SWP_NOACTIVATE);
                Pump(120);
            }

            GetWindowRect(g_win, &now);
            g_resizeKept = ((now.right - now.left) >= uw - 24 &&
                            (now.bottom - now.top) >= uh - 24) ? 1 : 0;
            Say("resize: after app re-apply -> %dx%d@(%d,%d) kept=%d",
                (int)(now.right - now.left), (int)(now.bottom - now.top),
                (int)now.left, (int)now.top, g_resizeKept);
        }
    }

    Say("SUMMARY polls=%d top_set=%d above_probe=%d resize_kept=%d",
        g_polls, g_topSet, g_aboveN, g_resizeKept);
    Say("VERDICT %s",
        (g_topSet == 0 && g_aboveN == 0 && g_resizeKept)
            ? "FIXED (topmost stripped, z-order frozen, user resize kept)"
            : "NOT FIXED");

    ok = (g_topSet == 0 && g_aboveN == 0 && g_resizeKept) ? 0 : 1;

    DestroyWindow(g_win);
    DestroyWindow(g_ref);
    if (g_logFile) fclose(g_logFile);
    return ok;
}
