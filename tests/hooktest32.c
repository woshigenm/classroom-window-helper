/*
 * hooktest32.c —— bypass_hook32.dll 的自测载体 (32 位)
 *
 * 模拟 MultiRender.exe 的行为, 每 500ms:
 *   1) SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 屏宽, 屏高, 0)
 *   2) SetWindowLongW(hwnd, GWL_STYLE, style | WS_POPUP)
 *   3) ShowWindow(hwnd, SW_SHOWMAXIMIZED)
 * 每一步之后都记一次窗口矩形/样式, 这样能精确定位是"哪一步"把窗口变大的。
 *
 * 注入 bypass_hook32.dll 之后, 1/2/3 都应该被改写成小窗。
 *
 * 结果写 hooktest_result.txt。
 *
 * 编译:
 *   i686-w64-mingw32-gcc -std=gnu11 -O2 hooktest32.c -o hooktest32.exe -lgdi32 -luser32
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

static volatile LONG g_stop = 0;
static HWND          g_hwnd = NULL;
static HWND          g_hwnd2 = NULL;   /* 模拟「虚拟桌面」预览窗: 75% 屏 + 置顶 + 带框 */
static HWND          g_hwnd3 = NULL;   /* 模拟 StudentHelperWnd: 学生自己的界面, 满屏也不该被降级 */
static int           g_sw, g_sh;
static int           g_noMinTrack;   /* 1 = 不把最小尺寸钉成整屏 (按身份测降级用) */
static char          g_resPath[MAX_PATH] = "hooktest_result.txt";

/* 结果文件固定写到 exe 同目录 —— 不依赖工作目录 (用 start 启动时 CWD 可能不是桌面) */
static void InitResultPath(void)
{
    char dir[MAX_PATH];
    char* p;

    g_resPath[0] = '\0';
    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    p = strrchr(dir, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(g_resPath, sizeof(g_resPath) - 1, "%shooktest_result.txt", dir);
    g_resPath[sizeof(g_resPath) - 1] = '\0';
}

/* 等待, 但要继续抽消息 —— 跨线程的 SetWindowPos/SetWindowLong 会
   SendMessage 给窗口线程, 窗口线程如果只是 Sleep 不抽消息就会互相卡住
   (真实渲染器的主循环一直在抽消息, 所以不会遇到)。 */
static void PumpSleep(DWORD ms, MSG* msg)
{
    DWORD t0 = GetTickCount();
    while (GetTickCount() - t0 < ms) {
        while (PeekMessageA(msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(msg);
            DispatchMessageA(msg);
        }
        Sleep(10);
    }
}

static void Snapshot(const char* tag)
{
    RECT  r = {0};
    LONG  st, ex;
    FILE* f;

    GetWindowRect(g_hwnd, &r);
    st = GetWindowLongW(g_hwnd, GWL_STYLE);
    ex = GetWindowLongW(g_hwnd, GWL_EXSTYLE);

    f = fopen(g_resPath, "a");
    if (!f) return;
    fprintf(f, "%-16s (%ld,%ld)-(%ld,%ld) %ldx%ld  top=%s popup=%s\n",
            tag,
            (long)r.left, (long)r.top, (long)r.right, (long)r.bottom,
            (long)(r.right - r.left), (long)(r.bottom - r.top),
            (ex & WS_EX_TOPMOST) ? "Y" : "-",
            (st & WS_POPUP) ? "Y" : "-");
    fclose(f);
}

/* 模拟渲染器: 每 500ms 重新置顶+改样式+最大化 (老版本, 稳定性已知) */
static DWORD WINAPI RendererProc(LPVOID p)
{
    int  n = 0;
    char tag[64];
    LONG st;
    FILE* f;

    (void)p;
    while (!g_stop) {
        n++;

        f = fopen(g_resPath, "a");
        if (f) {
            fprintf(f, "iter%03d-IAT  SetWindowPos->%p SetWindowLongW->%p ShowWindow->%p\n",
                    n, *(void**)&SetWindowPos, *(void**)&SetWindowLongW,
                    *(void**)&ShowWindow);
            fclose(f);
        }

        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_sw, g_sh, 0);
        _snprintf(tag, sizeof(tag) - 1, "iter%03d-afterSWP", n);
        tag[sizeof(tag) - 1] = '\0';
        Snapshot(tag);

        st = GetWindowLongW(g_hwnd, GWL_STYLE);
        st &= ~(LONG)0x00C40000;
        st |= (LONG)0x80000000;
        SetWindowLongW(g_hwnd, GWL_STYLE, st);
        _snprintf(tag, sizeof(tag) - 1, "iter%03d-afterSWL", n);
        tag[sizeof(tag) - 1] = '\0';
        Snapshot(tag);

        ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
        _snprintf(tag, sizeof(tag) - 1, "iter%03d-afterShow", n);
        tag[sizeof(tag) - 1] = '\0';
        Snapshot(tag);

        if (g_hwnd2) {
            RECT v; LONG st2; FILE* fv;
            GetWindowRect(g_hwnd2, &v);
            st2 = GetWindowLongW(g_hwnd2, GWL_STYLE);
            fv = fopen(g_resPath, "a");
            if (fv) {
                fprintf(fv, "vdesk%03d        %ldx%ld cap=%s\n", n,
                        (long)(v.right - v.left), (long)(v.bottom - v.top),
                        (st2 & WS_CAPTION) ? "Y" : "-");
                fclose(fv);
            }
        }
        if (g_hwnd3) {
            RECT v; FILE* fv;
            GetWindowRect(g_hwnd3, &v);
            fv = fopen(g_resPath, "a");
            if (fv) {
                fprintf(fv, "helper%03d       %ldx%ld\n", n,
                        (long)(v.right - v.left), (long)(v.bottom - v.top));
                fclose(fv);
            }
        }

        Sleep(500);
    }
    return 0;
}

/* 模拟真实渲染器: 在 WM_GETMINMAXINFO 里把"最小尺寸"钉成整个桌面。
   实测 MultiRenderWindowClass 就是这样 —— 我们 SetWindowPos 要 852x479,
   窗口管理器按这个下限夹回 2558x1286, 而且不产生任何 SetWindowPos 调用。 */
static LRESULT CALLBACK TestWndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_GETMINMAXINFO && !g_noMinTrack) {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        mmi->ptMinTrackSize.x = g_sw;
        mmi->ptMinTrackSize.y = g_sh;
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int main(void)
{
    WNDCLASSA wc;
    MSG       msg;
    HANDLE    th;
    DWORD     t0;
    FILE*     f;
    const char* cmd = GetCommandLineA();
    int  noLoop = (cmd && strstr(cmd, "--novd")) ? 1 : 0;   /* 不跑"变全屏"的渲染循环 */
    int  seq    = (cmd && strstr(cmd, "--seq")) ? 1 : 0;     /* 跑"全屏→最大化→应用拉回→还原"序列 */
    int  cw, ch;

    InitResultPath();

    g_noMinTrack = noLoop;

    f = fopen(g_resPath, "w");
    if (f) {
        fprintf(f, "screen=%dx%d\n", GetSystemMetrics(SM_CXSCREEN),
                GetSystemMetrics(SM_CYSCREEN));
        fclose(f);
    }

    g_sw = GetSystemMetrics(SM_CXSCREEN);
    g_sh = GetSystemMetrics(SM_CYSCREEN);

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = TestWndProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "MultiRenderWindowClass";   /* 用真实渲染器类名, 才会走"广播窗"逻辑 */
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    if (!RegisterClassA(&wc)) {
        FILE* fe = fopen(g_resPath, "a");
        if (fe) { fprintf(fe, "RegisterClass failed err=%lu cls=%s\n",
                          GetLastError(), wc.lpszClassName); fclose(fe); }
        return 1;
    }

    /* 第二个类: 模拟教室软件的「虚拟桌面」窗口 —— 普通 DefWindowProc,
       没有"最小尺寸钉死成全屏"那套把戏。 */
    {
        WNDCLASSA wc2 = wc;
        wc2.lpfnWndProc   = DefWindowProcA;
        wc2.lpszClassName = "HookTestVDesk";
        RegisterClassA(&wc2);
    }

    /* 第三个类: 学生端自己的界面窗口 (实测类名 StudentHelperWnd.oseasy) */
    {
        WNDCLASSA wc3 = wc;
        wc3.lpfnWndProc   = DefWindowProcA;
        wc3.lpszClassName = "StudentHelperWnd.oseasy";
        RegisterClassA(&wc3);
    }

    g_hwnd = CreateWindowExA(0, "MultiRenderWindowClass", "hooktest window",
                             WS_OVERLAPPEDWINDOW,
                             100, 100,
                             noLoop ? g_sw * 3 / 4 : 800,
                             noLoop ? g_sh * 3 / 4 : 600,
                             NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        MessageBoxA(NULL, "CreateWindow failed", "hooktest", MB_OK);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOWNORMAL);
    UpdateWindow(g_hwnd);
    Snapshot("000-before");

    if (seq) {
        int i;
        Snapshot("seq-enter");
        PumpSleep(3000, &msg);                        /* 等注入完成 */
        Snapshot("seq-0-injected");

        SetWindowPos(g_hwnd, HWND_TOPMOST, 0, 0, g_sw, g_sh, SWP_FRAMECHANGED);
        PumpSleep(400, &msg); Snapshot("seq-1-appfull");

        ShowWindow(g_hwnd, SW_SHOWMAXIMIZED);
        PumpSleep(600, &msg); Snapshot("seq-2-max");

        /* 应用想把窗口按回它缓存的小尺寸 —— 必须被挡 */
        SetWindowPos(g_hwnd, HWND_TOPMOST, 159, 160, 960, 900, SWP_FRAMECHANGED);
        PumpSleep(400, &msg); Snapshot("seq-3-pullback");

        /* 用户点"还原" —— 必须真的还原成用户矩形 */
        ShowWindow(g_hwnd, SW_RESTORE);
        PumpSleep(600, &msg); Snapshot("seq-4-restore");

        /* 用户拖动: 用 WM_ENTERSIZEMOVE/EXITSIZEMOVE 包一次真实移动模拟 */
        SendMessageA(g_hwnd, WM_ENTERSIZEMOVE, 0, 0);
        SetWindowPos(g_hwnd, NULL, 300, 300, 700, 600, SWP_NOZORDER);
        SendMessageA(g_hwnd, WM_EXITSIZEMOVE, 0, 0);
        PumpSleep(400, &msg); Snapshot("seq-5-usersized");

        /* 应用又想按回去 —— 必须被挡 */
        SetWindowPos(g_hwnd, HWND_TOPMOST, 159, 160, 960, 900, SWP_FRAMECHANGED);
        PumpSleep(400, &msg); Snapshot("seq-6-afterapp");
        for (i = 0; i < 10; i++) { PumpSleep(100, &msg); }
        Snapshot("seq-7-final");
        return 0;
    }

    /* 再开一个「虚拟桌面」风格的窗口: 3/4 屏 + 带框 + 置顶。
       教室软件的虚拟桌面预览窗就是这个形状 (实测 1344x637 @1792x850),
       它绝对不能被我们降级成小窗。 */
    g_hwnd2 = CreateWindowExA(WS_EX_TOPMOST, "HookTestVDesk", "vdesk preview",
                              WS_OVERLAPPEDWINDOW,
                              50, 50, g_sw * 3 / 4, g_sh * 3 / 4,
                              NULL, NULL, wc.hInstance, NULL);
    if (g_hwnd2) {
        ShowWindow(g_hwnd2, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hwnd2);
    }

    /* 学生端自己的界面窗口: 这里故意让它满屏 (最坏情况),
       用来验证 "类名不在渲染器白名单里 -> 无论多大都不降级"。 */
    g_hwnd3 = CreateWindowExA(0, "StudentHelperWnd.oseasy", "student helper",
                              WS_OVERLAPPEDWINDOW,
                              0, 0, g_sw, g_sh,
                              NULL, NULL, wc.hInstance, NULL);
    if (g_hwnd3) {
        ShowWindow(g_hwnd3, SW_SHOWNOACTIVATE);
        UpdateWindow(g_hwnd3);
    }

    th = noLoop ? NULL : CreateThread(NULL, 0, RendererProc, NULL, 0, NULL);
    /* 40 秒: 给外部注入留时间, 并让 4 个阶段各跑两轮 */
    t0 = GetTickCount();
    while (GetTickCount() - t0 < 40000) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (noLoop && (GetTickCount() - t0) % 500 < 15) {
            RECT v; FILE* fv;
            /* 模拟真实渲染器: 每 500ms 既想还原 (SW_RESTORE), 又把它缓存的
               旧样式写回去 (没有 WS_MAXIMIZE) —— 两条路都可能把最大化清掉。 */
            LONG st2 = GetWindowLongW(g_hwnd, GWL_STYLE);
            ShowWindow(g_hwnd, SW_RESTORE);
            SetWindowLongW(g_hwnd, GWL_STYLE, (st2 & ~(LONG)WS_MAXIMIZE));
            SetWindowPos(g_hwnd, HWND_TOPMOST, 1666, 766, 868, 456, SWP_FRAMECHANGED);
            GetWindowRect(g_hwnd, &v);
            fv = fopen(g_resPath, "a");
            if (fv) {
                fprintf(fv, "idonly       %ldx%ld cap=%s\n",
                        (long)(v.right - v.left), (long)(v.bottom - v.top),
                        (GetWindowLongW(g_hwnd, GWL_STYLE) & WS_CAPTION) ? "Y" : "-");
                fclose(fv);
            }
        }
        Sleep(10);
    }

    InterlockedExchange(&g_stop, 1);
    if (th) WaitForSingleObject(th, 3000);
    Snapshot("999-final");

    DestroyWindow(g_hwnd);
    return 0;
}
