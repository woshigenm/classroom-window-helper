/* ============================================================
 *  screenguard_test.c -- 屏幕监控守卫的自测载体
 *
 *  模仿 StudentLogic.dll 的真实抓屏路径 (REA/Ghidra 实证):
 *      CreateDCW(L"DISPLAY") -> CreateCompatibleDC/Bitmap
 *      -> BitBlt(..., SRCCOPY|CAPTUREBLT) -> GetDIBits
 *
 *  流程:
 *    1) 用颜色 A 填满自己的窗口;
 *    2) 抓一屏, 记指纹;
 *    3) 换成颜色 B, 再抓一屏, 记指纹;
 *    4) 两次指纹相同 = 第二次抓到的还是"定格帧"(守卫生效);
 *       不同 = 抓到的是实时桌面。
 *
 *  用法: screenguard_test.exe [--inject <bypass_hook32.dll>] [<inject32.exe>]
 *  配合 tests\screenguard_test.ps1 使用 (它会写 bin\bypass_screen.ini)。
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 类名不能以 "MultiRender" 开头: bypass_hook 的 IsRenderWindowClass() 就是按
   这个前缀认课堂渲染窗口的, 命中后它会把本窗口当"广播窗口"接管 —— 包括把
   SetWindowPos(HWND_TOPMOST) 改写成 HWND_NOTOPMOST (bypass_hook.c:1748)。
   那样本测试窗口会被别的前台窗口盖住, 守卫关着也会抓到同一幅画面, 报出假的
   frozen=1。这是本文件改名前的真实故障(全套里 1/10 次 off=1, 且 ProbeVisible
   打出的 diag 显示 center_hit 落在了别的窗口上)。 */
#define RENDER_CLASS "ScreenguardProbeWnd"

static HWND   g_win;
static COLORREF g_color;

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC  dc = BeginPaint(h, &ps);
        RECT rc;
        HBRUSH br;

        GetClientRect(h, &rc);
        br = CreateSolidBrush(g_color);
        FillRect(dc, &rc, br);
        DeleteObject(br);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void Pump(DWORD ms)
{
    DWORD end = GetTickCount() + ms;

    while ((LONG)(end - GetTickCount()) > 0) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        Sleep(10);
    }
}

static int RunInjector(const char* injector, const char* dll)
{
    char cmd[1024];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD rc;

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

/* 和 StudentLogic.dll 的 FUN_10025610 一样的抓屏顺序 */
static DWORD CaptureRectSum(int x, int y, int w, int h)
{
    HDC        disp;
    HDC        mem;
    HBITMAP    bm;
    HGDIOBJ    old;
    BYTE*      bits;
    BITMAPINFO bmi;
    DWORD      sum = 0x811C9DC5u;
    int        i;

    disp = CreateDCW(L"DISPLAY", NULL, NULL, NULL);
    if (!disp) return 0;
    mem = CreateCompatibleDC(disp);
    bm  = CreateCompatibleBitmap(disp, w, h);
    old = SelectObject(mem, bm);

    BitBlt(mem, 0, 0, w, h, disp, x, y, SRCCOPY | CAPTUREBLT);

    bits = (BYTE*)calloc(1, (size_t)w * (size_t)h * 4);
    if (bits) {
        memset(&bmi, 0, sizeof(bmi));
        bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth       = w;
        bmi.bmiHeader.biHeight      = -h;
        bmi.bmiHeader.biPlanes      = 1;
        bmi.bmiHeader.biBitCount    = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        GetDIBits(mem, bm, 0, (UINT)h, bits, &bmi, DIB_RGB_COLORS);
        for (i = 0; i < w * h * 4; i += 997)
            sum = (sum ^ bits[i]) * 16777619u;
        free(bits);
    }

    SelectObject(mem, old);
    DeleteObject(bm);
    DeleteDC(mem);
    DeleteDC(disp);
    return sum;
}

static DWORD CaptureChecksum(int w, int h)
{
    return CaptureRectSum(0, 0, w, h);
}

/* --affinity: 验证 SetWindowDisplayAffinity 在本机是否真的把窗口挡在抓屏之外。
   控制组(不设)应"可见/隐藏"两次抓屏不同; 实验组(设了 WDA)应完全相同
   (说明抓屏里根本没有这个窗口)。 */
static int CheckAffinity(void)
{
    int   sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    RECT  r;
    DWORD a1, b1, a2, b2, aff = 0;
    HWND  w1, w2;
    int   control, excluded;

    g_color = RGB(240, 235, 60);
    w1 = CreateWindowExA(0, RENDER_CLASS, "control window",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         sw / 4, sh / 4, 480, 360, NULL, NULL,
                         GetModuleHandleA(NULL), NULL);
    if (!w1) { printf("control window failed\n"); return 1; }
    ShowWindow(w1, SW_SHOW);
    /* 抓屏区域必须真的被这个窗口覆盖住: 放到最上面, 否则前台终端会挡住,
       "visible" 和 "hidden" 两次抓到的是同一个画面 -> 误报 FAIL */
    SetWindowPos(w1, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(w1);
    Pump(500);
    GetWindowRect(w1, &r);
    a1 = CaptureRectSum(r.left, r.top, r.right - r.left, r.bottom - r.top);
    ShowWindow(w1, SW_HIDE);
    Pump(500);
    b1 = CaptureRectSum(r.left, r.top, r.right - r.left, r.bottom - r.top);
    control = (a1 != b1) ? 1 : 0;
    printf("control  visible=%08lx hidden=%08lx changed=%d\n",
           (unsigned long)a1, (unsigned long)b1, control);
    DestroyWindow(w1);
    Pump(300);

    w2 = CreateWindowExA(0, RENDER_CLASS, "excluded window",
                         WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                         sw / 4, sh / 4, 480, 360, NULL, NULL,
                         GetModuleHandleA(NULL), NULL);
    if (!w2) { printf("excluded window failed\n"); return 2; }
    if (!SetWindowDisplayAffinity(w2, WDA_EXCLUDEFROMCAPTURE))
        SetWindowDisplayAffinity(w2, WDA_MONITOR);
    GetWindowDisplayAffinity(w2, &aff);
    ShowWindow(w2, SW_SHOW);
    SetWindowPos(w2, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(w2);
    Pump(500);
    GetWindowRect(w2, &r);
    a2 = CaptureRectSum(r.left, r.top, r.right - r.left, r.bottom - r.top);
    ShowWindow(w2, SW_HIDE);
    Pump(500);
    b2 = CaptureRectSum(r.left, r.top, r.right - r.left, r.bottom - r.top);
    excluded = (a2 == b2) ? 1 : 0;
    printf("wda      affinity=0x%lx visible=%08lx hidden=%08lx excluded=%d\n",
           (unsigned long)aff, (unsigned long)a2, (unsigned long)b2, excluded);
    DestroyWindow(w2);

    printf("SUMMARY control_changed=%d affinity_excluded=%d\n", control, excluded);
    printf("VERDICT %s\n", (control && excluded)
           ? "PASS (SetWindowDisplayAffinity keeps our window out of captures)"
           : "FAIL");
    return (control && excluded) ? 0 : 3;
}

/* 这条自测的判据是"窗口换了颜色, 抓屏也跟着换", 前提是窗口真的显示在实时
   桌面上。只要它被别的窗口盖住, 两次抓屏抓到的就是同一幅画面, 守卫关着也会
   报 frozen=1 (CheckAffinity 早就为这个把窗口提到最上层)。所以主流程也提到
   最上层, 并在抓屏前验证窗口中心确实命中自己; 不命中就打印环境并判 INVALID,
   而不是把"没看到窗口"说成"画面被定格"。 */
static int ProbeVisible(HWND w)
{
    RECT  r;
    POINT c;
    HWND  hit;
    HWND  fg = GetForegroundWindow();
    HDESK desk;
    char  cls[128] = "?";
    char  title[160] = "?";
    int   ours;

    GetWindowRect(w, &r);
    c.x = (r.left + r.right) / 2;
    c.y = (r.top + r.bottom) / 2;
    hit = WindowFromPoint(c);
    ours = (hit == w || (hit && IsChild(w, hit))) ? 1 : 0;
    if (fg) {
        GetClassNameA(fg, cls, sizeof(cls) - 1);
        GetWindowTextA(fg, title, sizeof(title) - 1);
    }
    desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    printf("diag: rect=%ld,%ld,%ld,%ld center_hit=%p ours=%d fg=%p "
           "class=%s title=%.40s input_desktop=%s\n",
           r.left, r.top, r.right, r.bottom, (void *)hit, ours, (void *)fg,
           cls, title, desk ? "ok" : "unavailable");
    if (desk) CloseDesktop(desk);
    return ours;
}

int main(int argc, char** argv)
{
    const char* dll = NULL;
    const char* injector = NULL;
    int         affinity = 0;
    WNDCLASSA wc;
    MSG msg;
    int sw, sh, i;
    DWORD sumA, sumB;
    int frozen;

    for (i = 1; i < argc; i++) {
        if (!_stricmp(argv[i], "--inject") && i + 1 < argc) dll = argv[++i];
        else if (!_stricmp(argv[i], "--affinity")) affinity = 1;
        else if (!injector) injector = argv[i];
    }

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.hCursor       = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = RENDER_CLASS;
    if (!RegisterClassA(&wc)) { printf("RegisterClass failed\n"); return 1; }

    if (affinity) return CheckAffinity();

    g_color = RGB(10, 40, 90);
    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    g_win = CreateWindowExA(0, RENDER_CLASS, "screenguard test",
                            WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                            sw / 6, sh / 6, sw / 2, sh / 2,
                            NULL, NULL, wc.hInstance, NULL);
    if (!g_win) { printf("CreateWindow failed\n"); return 2; }
    ShowWindow(g_win, SW_SHOW);
    UpdateWindow(g_win);
    Pump(400);

    if (dll) {
        char buf[MAX_PATH];
        if (!injector) {
            char exe[MAX_PATH] = "";
            char* p;
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            p = strrchr(exe, '\\');
            if (p) *(p + 1) = '\0';
            _snprintf(buf, sizeof(buf) - 1, "%sinject32.exe", exe);
            injector = buf;
        }
        if (!RunInjector(injector, dll))
            printf("WARNING: injector failed, continuing as baseline\n");
        Pump(1800);          /* Worker 起来 + 读一次 ini */
    }

    /* 提到最上层 + 先验证可见性, 理由见 ProbeVisible 上方。 */
    SetWindowPos(g_win, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    UpdateWindow(g_win);
    Pump(200);
    if (!ProbeVisible(g_win)) {
        printf("VERDICT INVALID (probe window is not on the live desktop)\n");
        DestroyWindow(g_win);
        return 4;
    }

    /* 颜色 A -> 第一次抓屏 (守卫会把它存成定格帧) */
    g_color = RGB(10, 40, 90);
    InvalidateRect(g_win, NULL, TRUE);
    Pump(300);
    sumA = CaptureChecksum(sw, sh);
    printf("capture#1 (color A) -> %08lx\n", (unsigned long)sumA);
    Pump(300);

    /* 换成颜色 B -> 第二次抓屏 */
    g_color = RGB(200, 30, 30);
    InvalidateRect(g_win, NULL, TRUE);
    Pump(300);
    sumB = CaptureChecksum(sw, sh);
    printf("capture#2 (color B) -> %08lx\n", (unsigned long)sumB);

    frozen = (sumA == sumB) ? 1 : 0;
    printf("SUMMARY first=%08lx second=%08lx frozen=%d\n",
           (unsigned long)sumA, (unsigned long)sumB, frozen);
    printf("VERDICT %s\n", frozen ? "FROZEN (2nd capture is the cached frame)"
                                  : "LIVE (2nd capture shows the new color)");

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    DestroyWindow(g_win);
    return 0;   /* 退出码恒 0: 判据在 SUMMARY, 由脚本读 */
}
