/* ============================================================
 *  hangwin_test.c -- "对端不泵消息" 的受害者进程 (只用于跨进程兜底测试)
 *
 *  用途: 验证 bypass_hook32.dll 里的跨进程兜底 (FixForeignGeometry) 不会把
 *  enforcer 线程卡死。它创建一个渲染器类名 (MultiRender*) + 95% 屏的顶层
 *  窗口, 然后 **挂起自己的主线程** —— 从此它不再取任何消息, 任何发给它的
 *  同步消息 (SendMessage / SetWindowLong) 都会永远等下去。
 *
 *  测试脚本 (tests/foreign_fix_test.ps1) 会把它复制成 ScreenRender.exe 再跑,
 *  这样注入方才会把它当成"同族课堂进程"去做跨进程兜底。
 *
 *  用法: hangwin_test.exe [--stay <秒>]
 *  启动后写 victim.log ("ready hwnd=... pid=..."), 然后自挂起。
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLS "MultiRenderHangTest"

static LRESULT CALLBACK VictimProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

int main(int argc, char** argv)
{
    WNDCLASSA wc;
    HWND      win;
    MSG       msg;
    int       sw, sh, i;
    FILE*     f;

    (void)argc;
    (void)argv;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = VictimProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLS;
    if (!RegisterClassA(&wc)) {
        printf("RegisterClass failed err=%lu\n", (unsigned long)GetLastError());
        return 1;
    }

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    win = CreateWindowExA(0, CLS, "foreign fix victim (suspended)",
                          WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                          sw / 40, sh / 40, sw * 95 / 100, sh * 95 / 100,
                          NULL, NULL, wc.hInstance, NULL);
    if (!win) {
        printf("CreateWindowEx failed err=%lu\n", (unsigned long)GetLastError());
        return 2;
    }

    /* Start-Process -WindowStyle Hidden 会把 STARTUPINFO 的 wShowWindow 设成
       SW_HIDE, 于是 WS_VISIBLE 建出来的窗口也可能是隐藏状态 —— 显式 Show 一次
       (探针实测 IsWindowVisible=0 时, 注入方的 enforcer 会在第一道
       "不可见就跳过"上直接 return, 跨进程兜底根本跑不到)。 */
    ShowWindow(win, SW_SHOW);
    UpdateWindow(win);

    /* 让窗口真正显示/上屏, 再停止泵消息 */
    for (i = 0; i < 30; i++) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }

    f = fopen("victim.log", "w");
    if (f) {
        fprintf(f, "ready hwnd=%p pid=%lu bits=%d\n", (void*)win,
                (unsigned long)GetCurrentProcessId(), (int)(sizeof(void*) * 8));
        fclose(f);
    }
    printf("victim ready hwnd=%p pid=%lu (suspending main thread)\n",
           (void*)win, (unsigned long)GetCurrentProcessId());
    fflush(stdout);

    /* ★ 从这一刻起不再取消息: 任何同步 SendMessage 都会永远挂住。
       这正是"对端 UI 线程卡死"的最小复现。 */
    SuspendThread(GetCurrentThread());

    for (;;) Sleep(1000);   /* 挂起后不会走到这里; 恢复也不会退出 */
    return 0;
}
