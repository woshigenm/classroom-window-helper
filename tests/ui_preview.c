/*
 * ui_preview.c -- test harness: shows the real main window without the admin
 * check, so the UI paths (buttons / tray / status report) can be driven by the
 * PowerShell suites (reload, version self-check, multi-instance).
 *
 * It includes bypass.c itself, renames its WinMain away and provides a minimal
 * one that only creates the window and pumps messages.
 *
 *   ui_preview.exe [--seconds N]     auto-exit after N seconds (default: never)
 *
 * Build: make.bat uitest   ->   bin\ui_preview.exe   (64-bit, same as bypass.exe)
 */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0603

#define WinMain Bypass_WinMain_unused
#include "../src/bypass.c"
#undef WinMain

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrev, LPSTR cmd, int show)
{
    WNDCLASSEXW wc;
    MSG         msg;
    HWND        hwnd;
    int         seconds = 0;

    (void)hPrev; (void)show;
    if (cmd && strstr(cmd, "--seconds")) {
        const char* p = strstr(cmd, "--seconds");
        seconds = atoi(p + 9);
    }

    StateInit();
    ShutGuardLoad();
    InitDpiApis();
    g_dpi = GetWindowDpiSafe(NULL);

    {
        INITCOMMONCONTROLSEX icex;
        icex.dwSize = sizeof(icex);
        icex.dwICC = ICC_WIN95_CLASSES;
        InitCommonControlsEx(&icex);
    }

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_SHIELD);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIconSm = LoadIcon(NULL, IDI_SHIELD);
    if (!RegisterClassExW(&wc)) return 1;

    hwnd = CreateWindowExW(WS_EX_APPWINDOW, WINDOW_CLASS, WINDOW_TITLE,
        WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, S(712), S(748),
        NULL, NULL, hInstance, NULL);
    if (!hwnd) return 1;

    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);

    if (seconds > 0) SetTimer(hwnd, 0x7EEE, (UINT)seconds * 1000, NULL);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_TIMER && msg.wParam == 0x7EEE) break;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
