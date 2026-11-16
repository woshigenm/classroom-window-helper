/*
 * bypass_screen.inc -- keep the classroom monitor/record path from seeing the
 * live desktop: serve a frozen (first) frame instead.
 *
 * Evidence (REA / Ghidra, StudentLogic.dll sha256 404b4d..9d84):
 *   FUN_10025610 @ 0x10025610  (called from FUN_10025720)
 *       CreateDCW(L"DISPLAY") -> CreateCompatibleDC/Bitmap
 *       BitBlt(mem, 0,0,w,h, display, 0,0, SRCCOPY|CAPTUREBLT)
 *       GetDIBits(mem, hbm, 0, h, bits, &bmi, 0)     -> pixels for the upload
 *   So the single interception point is BitBlt whose source DC is a screen DC.
 *
 * Behaviour:
 *   mode=freeze  first capture seeds a snapshot, every later capture is served
 *                from that snapshot (the screen is never read again)
 *   mode=blank   never capture at all, serve a solid frame
 *   enabled=0    no interception (default)
 *
 * The config is bin\bypass_screen.ini next to the DLL, re-read at most twice a
 * second, so the main program can toggle it without restarting anything.
 *
 * Pure ASCII on purpose (this file is #included by bypass_hook.c).
 */

#define SC_INI_NAME      "bypass_screen.ini"
#define SC_MAX_DC        16
#define SC_MAX_W         8192
#define SC_MAX_H         8192
#define SC_LOG_MS        3000

static volatile LONG g_scEnabled;   /* 0 = off (default), 1 = on */
static volatile LONG g_scMode;      /* 0 = freeze, 1 = blank */
static HDC           g_scDCs[SC_MAX_DC];
static volatile LONG g_scDcNext;
static CRITICAL_SECTION g_scCs;
static int           g_scCsReady;

static BYTE*      g_scBits;         /* frozen frame, 32bpp top-down */
static BITMAPINFO g_scBmi;
static int        g_scW, g_scH;     /* cached frame size */
static int        g_scSeeded;
static DWORD      g_scCfgTick, g_scLastLog, g_scServed, g_scSeededTick;
static HANDLE     g_scLogH;

/* ---------------- logging (ASCII, same file as the rest of the DLL) -------- */

static void SC_Log(const char* fmt, ...)
{
    char       line[512];
    char       dir[MAX_PATH], path[MAX_PATH];
    char*      p;
    SYSTEMTIME st;
    va_list    ap;
    int        n;
    DWORD      wrote = 0;

    if (g_scLogH == INVALID_HANDLE_VALUE || !g_scLogH) {
        if (!GetModuleFileNameA(g_self, dir, sizeof(dir))) return;
        p = strrchr(dir, '\\');
        if (!p) return;
        *(p + 1) = '\0';
        _snprintf(path, sizeof(path) - 1, "%sbypass_hook.log", dir);
        path[sizeof(path) - 1] = '\0';
        g_scLogH = CreateFileA(path, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_scLogH == INVALID_HANDLE_VALUE) return;
    }

    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line) - 3, "[%02d:%02d:%02d.%03d] [pid %lu] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  (unsigned long)GetCurrentProcessId());
    if (n < 0) return;
    va_start(ap, fmt);
    _vsnprintf(line + n, sizeof(line) - n - 3, fmt, ap);
    va_end(ap);
    line[sizeof(line) - 3] = '\0';
    n = (int)strlen(line);
    line[n++] = '\r';
    line[n++] = '\n';
    WriteFile(g_scLogH, line, (DWORD)n, &wrote, NULL);
}

/* ---------------- config: bin\bypass_screen.ini ---------------------------- */

static void SC_ReadConfig(void)
{
    char   dir[MAX_PATH], path[MAX_PATH];
    char*  p;
    FILE*  f;
    char   line[128];
    LONG   enabled = 0, mode = 0;

    if (GetTickCount() - g_scCfgTick < 500) return;
    g_scCfgTick = GetTickCount();

    if (!GetModuleFileNameA(g_self, dir, sizeof(dir))) return;
    p = strrchr(dir, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(path, sizeof(path) - 1, "%s" SC_INI_NAME, dir);
    path[sizeof(path) - 1] = '\0';

    f = fopen(path, "r");
    if (!f) {
        /* no file = feature off (the main program creates it when enabled) */
        InterlockedExchange(&g_scEnabled, 0);
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

        if (!_stricmp(k, "enabled"))      enabled = (atoi(v) != 0) ? 1 : 0;
        else if (!_stricmp(k, "mode"))    mode = (_stricmp(v, "blank") == 0) ? 1 : 0;
    }
    fclose(f);

    InterlockedExchange(&g_scMode, mode);
    InterlockedExchange(&g_scEnabled, enabled);
}

/* ---------------- screen DC bookkeeping ----------------------------------- */

static void SC_NoteDC(HDC hdc)
{
    LONG slot;

    if (!hdc || hdc == INVALID_HANDLE_VALUE || !g_scCsReady) return;
    slot = InterlockedIncrement(&g_scDcNext) & (SC_MAX_DC - 1);
    EnterCriticalSection(&g_scCs);
    g_scDCs[slot] = hdc;
    LeaveCriticalSection(&g_scCs);
}

/* Is this DC reading the real screen? Two independent signals:
      1) it is one we handed out from CreateDC("DISPLAY") / GetDC(NULL);
      2) its current bitmap is the 1x1 "screen" bitmap (a memory DC that the app
         drew into has a real bitmap, so it never matches). */
static BOOL SC_IsScreenDC(HDC hdc)
{
    int i;

    if (!hdc || !g_scCsReady) return FALSE;
    EnterCriticalSection(&g_scCs);
    for (i = 0; i < SC_MAX_DC; i++) {
        if (g_scDCs[i] == hdc) {
            LeaveCriticalSection(&g_scCs);
            return TRUE;
        }
    }
    LeaveCriticalSection(&g_scCs);

    if (GetDeviceCaps(hdc, TECHNOLOGY) != DT_RASDISPLAY) return FALSE;
    {
        HGDIOBJ hbm = GetCurrentObject(hdc, OBJ_BITMAP);
        BITMAP  bm;
        if (!hbm) return FALSE;
        memset(&bm, 0, sizeof(bm));
        if (GetObject(hbm, sizeof(bm), &bm) != sizeof(bm)) return FALSE;
        if (bm.bmWidth == 1 && bm.bmHeight == 1) return TRUE;
    }
    return FALSE;
}

/* ---------------- frozen frame -------------------------------------------- */

static void SC_DropFrameLocked(void)
{
    if (g_scBits) {
        free(g_scBits);
        g_scBits = NULL;
    }
    g_scW = g_scH = 0;
    g_scSeeded = 0;
}

/* Seed the cache from the pixels the caller just captured (the real BitBlt has
   already run). Only the first frame is ever taken from the live screen. */
static void SC_SeedFromDC(HDC dst)
{
    HGDIOBJ hbm;
    BITMAP  bm;
    int     w, h;
    BYTE*   buf;

    if (!g_scCsReady) return;
    hbm = GetCurrentObject(dst, OBJ_BITMAP);
    if (!hbm) return;
    memset(&bm, 0, sizeof(bm));
    if (GetObject(hbm, sizeof(bm), &bm) != sizeof(bm)) return;
    w = (int)bm.bmWidth;
    h = (int)bm.bmHeight;
    if (w < 16 || h < 16 || w > SC_MAX_W || h > SC_MAX_H) return;

    buf = (BYTE*)calloc(1, (size_t)w * (size_t)h * 4);
    if (!buf) return;

    EnterCriticalSection(&g_scCs);
    SC_DropFrameLocked();
    memset(&g_scBmi, 0, sizeof(g_scBmi));
    g_scBmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    g_scBmi.bmiHeader.biWidth       = w;
    g_scBmi.bmiHeader.biHeight      = -h;      /* top-down */
    g_scBmi.bmiHeader.biPlanes      = 1;
    g_scBmi.bmiHeader.biBitCount    = 32;
    g_scBmi.bmiHeader.biCompression = BI_RGB;
    g_scBmi.bmiHeader.biSizeImage   = w * h * 4;

    if (GetDIBits(dst, (HBITMAP)hbm, 0, (UINT)h, buf, &g_scBmi, DIB_RGB_COLORS) > 0) {
        g_scBits   = buf;
        g_scW      = w;
        g_scH      = h;
        g_scSeeded = 1;
        buf        = NULL;
    }
    LeaveCriticalSection(&g_scCs);
    if (buf) free(buf);
}

/* Serve the cached frame into the caller's bitmap. */
static BOOL SC_DrawFrozen(HDC dst, int x, int y, int w, int h, int sx, int sy)
{
    BOOL ok = FALSE;

    EnterCriticalSection(&g_scCs);
    if (g_scBits && w > 0 && h > 0) {
        if (sx < 0) sx = 0;
        if (sy < 0) sy = 0;
        if (sx + w > g_scW) w = g_scW - sx;
        if (sy + h > g_scH) h = g_scH - sy;
        if (w > 0 && h > 0) {
            int n = SetDIBitsToDevice(dst, x, y, (DWORD)w, (DWORD)h,
                                      sx, sy, 0, (UINT)g_scH,
                                      g_scBits, &g_scBmi, DIB_RGB_COLORS);
            ok = (n == h) ? TRUE : FALSE;
        }
    }
    LeaveCriticalSection(&g_scCs);
    return ok;
}

/* ---------------- hooks --------------------------------------------------- */

typedef BOOL (WINAPI *SC_PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef HDC  (WINAPI *SC_PFN_CreateDCW)(LPCWSTR, LPCWSTR, LPCWSTR, const DEVMODEW*);
typedef HDC  (WINAPI *SC_PFN_CreateDCA)(LPCSTR, LPCSTR, LPCSTR, const DEVMODE*);
typedef HDC  (WINAPI *SC_PFN_GetDC)(HWND);
typedef HDC  (WINAPI *SC_PFN_GetWindowDC)(HWND);

static SC_PFN_BitBlt      sc_BitBlt;
static SC_PFN_CreateDCW   sc_CreateDCW;
static SC_PFN_CreateDCA   sc_CreateDCA;
static SC_PFN_GetDC       sc_GetDC;
static SC_PFN_GetWindowDC sc_GetWindowDC;

static BOOL WINAPI SC_Hook_BitBlt(HDC dst, int x, int y, int w, int h,
                                  HDC src, int sx, int sy, DWORD rop)
{
    DWORD srccopy = rop & 0x00FFFFFF;

    if (!sc_BitBlt) return FALSE;
    if (g_scEnabled != 1 || !SC_IsScreenDC(src)) {
        return sc_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
    }
    /* only "copy pixels as-is" rops; AND/OR/XOR are not screen grabs */
    if (srccopy != SRCCOPY && srccopy != (SRCCOPY | CAPTUREBLT)) {
        return sc_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
    }
    if (w <= 0 || h <= 0) return sc_BitBlt(dst, x, y, w, h, src, sx, sy, rop);

    if (g_scMode == 1) {                 /* blank: never grab even the first frame */
        if (!g_scSeeded) {
            /* fill the cache with a solid colour at the destination size */
            HDC     mem = CreateCompatibleDC(dst);
            HGDIOBJ old = NULL;
            HBRUSH  br  = CreateSolidBrush(RGB(24, 26, 32));
            RECT    rc;

            if (mem) {
                HBITMAP bm = CreateCompatibleBitmap(dst, w, h);
                if (bm) old = SelectObject(mem, bm);
                SetRect(&rc, 0, 0, w, h);
                FillRect(mem, &rc, br);
                if (old) SelectObject(mem, old);
                SC_SeedFromDC(mem);
                if (bm) DeleteObject(bm);
                DeleteDC(mem);
            }
            DeleteObject(br);
        }
        if (SC_DrawFrozen(dst, x, y, w, h, sx, sy)) {
            if (GetTickCount() - g_scLastLog > SC_LOG_MS) {
                g_scLastLog = GetTickCount();
                SC_Log("[Screen] served %dx%d blank frame (mode=blank, %s)",
                       w, h, g_ownImage[0] ? g_ownImage : "?");
            }
            return TRUE;
        }
        return sc_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
    }

    /* freeze: the first grab seeds the cache; later grabs never read the screen */
    if (!g_scSeeded) {
        BOOL ok = sc_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
        if (ok) {
            SC_SeedFromDC(dst);
            if (g_scSeeded) {
                g_scSeededTick = GetTickCount();
                SC_Log("[Screen] froze the first frame (%dx%d) - later captures are served "
                       "from the cache (process=%s)", g_scW, g_scH,
                       g_ownImage[0] ? g_ownImage : "?");
            }
        }
        return ok;
    }

    if (SC_DrawFrozen(dst, x, y, w, h, sx, sy)) {
        g_scServed++;
        if (GetTickCount() - g_scLastLog > SC_LOG_MS) {
            g_scLastLog = GetTickCount();
            SC_Log("[Screen] capture replaced with the frozen frame (%dx%d at %d,%d; "
                   "served %lu)", w, h, sx, sy, (unsigned long)g_scServed);
        }
        return TRUE;
    }
    return sc_BitBlt(dst, x, y, w, h, src, sx, sy, rop);
}

static HDC WINAPI SC_Hook_CreateDCW(LPCWSTR drv, LPCWSTR dev, LPCWSTR port,
                                    const DEVMODEW* dm)
{
    HDC hdc = sc_CreateDCW ? sc_CreateDCW(drv, dev, port, dm) : NULL;
    if (hdc && drv && !_wcsicmp(drv, L"DISPLAY")) SC_NoteDC(hdc);
    return hdc;
}

static HDC WINAPI SC_Hook_CreateDCA(LPCSTR drv, LPCSTR dev, LPCSTR port,
                                    const DEVMODE* dm)
{
    HDC hdc = sc_CreateDCA ? sc_CreateDCA(drv, dev, port, dm) : NULL;
    if (hdc && drv && !_stricmp(drv, "DISPLAY")) SC_NoteDC(hdc);
    return hdc;
}

static HDC WINAPI SC_Hook_GetDC(HWND h)
{
    HDC hdc = sc_GetDC ? sc_GetDC(h) : NULL;
    if (hdc && (h == NULL || h == GetDesktopWindow())) SC_NoteDC(hdc);
    return hdc;
}

static HDC WINAPI SC_Hook_GetWindowDC(HWND h)
{
    HDC hdc = sc_GetWindowDC ? sc_GetWindowDC(h) : NULL;
    if (hdc && (h == NULL || h == GetDesktopWindow())) SC_NoteDC(hdc);
    return hdc;
}

/* Called from Worker(): resolve the real exports and start the config watch. */
static void SC_Init(void)
{
    HMODULE gdi, u32;

    if (!g_scCsReady) {
        InitializeCriticalSection(&g_scCs);
        g_scCsReady = 1;
    }

    gdi = GetModuleHandleA("gdi32.dll");
    u32 = GetModuleHandleA("user32.dll");
    if (gdi) {
        sc_BitBlt    = (SC_PFN_BitBlt)(void*)GetProcAddress(gdi, "BitBlt");
        sc_CreateDCW = (SC_PFN_CreateDCW)(void*)GetProcAddress(gdi, "CreateDCW");
        sc_CreateDCA = (SC_PFN_CreateDCA)(void*)GetProcAddress(gdi, "CreateDCA");
    }
    if (u32) {
        sc_GetDC       = (SC_PFN_GetDC)(void*)GetProcAddress(u32, "GetDC");
        sc_GetWindowDC = (SC_PFN_GetWindowDC)(void*)GetProcAddress(u32, "GetWindowDC");
    }

    SC_ReadConfig();
    SC_Log("[Screen] guard %s (mode=%s, hook=%s)",
           g_scEnabled ? "ON" : "off",
           g_scMode ? "blank" : "freeze",
           sc_BitBlt ? "ready" : "MISSING");
}

/* Called from the worker loop (cheap: re-reads the ini at most twice a second). */
static void SC_Tick(void)
{
    SC_ReadConfig();
}
