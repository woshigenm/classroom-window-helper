/*
 * bypass_hook.c —— 注入到课堂渲染进程 (32 位) 里的"反全屏"钩子 DLL
 *
 * 为什么用 IAT hook (依据反编译实证):
 *   MultiRender.exe  FUN_0040a790 / FUN_0040ac10
 *       SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, cx, cy, 0)
 *       SetWindowLongW(hwnd, GWL_STYLE, style & 0xFF3BFFFF | WS_POPUP)
 *       ShowCursor(0)
 *   MultiRender.exe  FUN_0040b780
 *       每次 ShowWindow 之后用 this+0x8ac 里存的 -1 重新套 HWND_TOPMOST
 *   ScreenRender.exe FUN_00411160
 *       500ms WM_TIMER 里 SetWindowPos(HWND_TOPMOST) 然后 KillTimer
 *   MultiClient.exe  FUN_004028a0
 *       SetWindowsHookEx(WH_MOUSE_LL) 丢掉所有 dwExtraInfo != 0x12452 的鼠标事件
 *
 * 这些调用全部走进程自己的 user32 导入表, 所以直接改它的 IAT 就能在
 * "它自己要变成全屏/置顶" 的那一刻把参数改掉 —— 不是我们从外面 200ms 抢一次,
 * 因此不会有反复重绘导致的闪屏。
 *
 * 关闭方法: 在本 DLL 同目录放一个名为 bypass_hook.off 的文件, 2 秒内全部放行。
 *
 * ⚠ 已知局限 (IAT hook 的固有边界):
 *   如果目标程序把 user32 导入表里的函数指针提前读进寄存器/局部变量,
 *   之后一直用那份旧地址调用 (-O2 的 GCC 就会这么做, 实测 32 位
 *   hooktest32.exe 的 SetWindowLongW/ShowWindow 就是被提到循环外的),
 *   那么注入后改 IAT 对它无效, 必须改成 inline hook (改 user32 函数入口)。
 *   厂商二进制 (MultiRender/ScreenRender) 由 MSVC 编译, 调用点通常是
 *   call dword ptr [__imp_xxx], 每次重新读 IAT, 所以 IAT hook 有效;
 *   万一实测无效, 说明要升级成 inline hook。
 *
 * 编译 (i686):
 *   i686-w64-mingw32-gcc -std=gnu11 -O2 -shared bypass_hook.c \
 *       -o bypass_hook32.dll -lpsapi -Wl,--kill-at
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <tlhelp32.h>
#include <string.h>

#include "bypass_state.h"

/* Windows 内部使用的窗口状态变更标志 (ShowWindow 内部 SetWindowPos 会带它),
   旧 MinGW 头文件里没有定义 */
#ifndef SWP_STATECHANGED
#define SWP_STATECHANGED 0x8000
#endif

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

static DWORD  g_pid;
static HMODULE g_self;
static volatile LONG g_disabled;
static volatile LONG g_patchCount;
static volatile LONG g_clampLogs;
static volatile LONG g_fixLogs;
static volatile LONG g_readLogs;
static volatile LONG g_mismatchLogs;
static volatile LONG g_deskLogs;
static volatile LONG g_hookLogs;
static volatile LONG g_procLogs;
static volatile LONG g_swpLogs;
static volatile LONG g_setwpLogs;
static volatile LONG g_showLogs;
static volatile LONG g_mwLogs;
static volatile LONG g_geoLogs2;
static volatile LONG g_capLogs;      /* 标题栏自动隐藏 */
static volatile LONG g_capCheckLogs; /* 定时器/悬停判定失败等 */
static volatile LONG g_capTraceLogs; /* 自动隐藏判定过程 (诊断, 有上限) */
static volatile LONG g_minLogs;      /* 最小化/重复最大化拦截 (避免刷屏) */
static volatile LONG g_sysCmdLogs;   /* WM_SYSCOMMAND 诊断日志 */
static volatile LONG g_reminimizing; /* 正在主动 re-minimize, 防止递归 */
static volatile LONG g_inMinimize;   /* 正在执行"最小化到任务栏"的 ShowWindow */
static DWORD         g_capTraceTick; /* 诊断日志节流 */

static int g_smallX, g_smallY, g_smallW, g_smallH;
static int g_maxW, g_maxH;

/* 小窗尺寸配置: bypass_config.ini 里的 small=
   "0.35"=占屏宽比例, "640x360"=写死外框尺寸。默认 屏宽/3,
   宽高比跟随源窗口, 免得视频被拉变形。 */
static int  g_cfgW, g_cfgH;

static char g_offPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static char g_configPath[MAX_PATH];  /* bypass_config.ini */
static char g_dllPath[MAX_PATH];     /* 本 DLL 的完整路径, 只用于日志诊断 */

/* 共享状态 (Local\BypassTool_State_v1) + 配置回退 */
static HANDLE       g_stateMap;
static HANDLE       g_stateEvent;
static BypassState* g_stateView;
static BypassState  g_stateSnap;
static int          g_stateFirst = 1;
static int          g_stWindow = 1;
static int          g_stTitle = 1;
static int          g_stMinTray = 0;
static int          g_stSmallW, g_stSmallH, g_stSmallPermille;
static DWORD        g_stateNextOpen;
static char g_ownImage[MAX_PATH] = "?";
static int  g_isBroadcastProc;        /* 1 = 本进程是全屏广播渲染器 (身份判定) */
static int  g_forcedWindowMode;       /* 1 = 命令行 fullscreen:1 被我们改写成 0 (而不是老师本来就发窗口模式) */
static char g_ownDesktop[128];        /* 本进程所在桌面名 (桌面守护用) */

/* 小窗矩形 / 配置 (定义在文件末尾) */
static void ComputeSmallRect(void);
static void SmallRectFor(int srcW, int srcH, int* px, int* py, int* pw, int* ph);
static void TrackGeo(HWND h, const RECT* r);
static void ForgetWindowGeom(HWND h);
static void FixOnOwnerThread(HWND h, int force);
static void SetUserRect(HWND h, const RECT* r);
static BOOL GetUserRect(HWND h, RECT* out);
static void EnsureWindowDesktop(void);
static void InitOwnDesktop(void);
static BOOL IsManaged(HWND h);
static BOOL LooksManaged(HWND h);
static void MarkManaged(HWND h);
static BOOL PidHasDll(DWORD pid);

/* 真正的 user32 函数 (用 GetProcAddress 拿, 绕开我们自己的 IAT) */
typedef BOOL     (WINAPI *PFN_SetWindowPos)(HWND, HWND, int, int, int, int, UINT);
typedef LONG     (WINAPI *PFN_SetWindowLongW)(HWND, int, LONG);
typedef LONG     (WINAPI *PFN_SetWindowLongA)(HWND, int, LONG);
typedef LONG_PTR (WINAPI *PFN_SetWindowLongPtrW)(HWND, int, LONG_PTR);
typedef LONG_PTR (WINAPI *PFN_SetWindowLongPtrA)(HWND, int, LONG_PTR);
typedef BOOL     (WINAPI *PFN_MoveWindow)(HWND, int, int, int, int, BOOL);
typedef BOOL     (WINAPI *PFN_SetWindowPlacement)(HWND, const WINDOWPLACEMENT*);
typedef BOOL     (WINAPI *PFN_ShowWindow)(HWND, int);
typedef BOOL     (WINAPI *PFN_ShowWindowAsync)(HWND, int);
typedef int      (WINAPI *PFN_ShowCursor)(BOOL);
typedef HCURSOR  (WINAPI *PFN_SetCursor)(HCURSOR);
typedef BOOL     (WINAPI *PFN_SwitchDesktop)(HDESK);
typedef HHOOK    (WINAPI *PFN_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);
typedef HHOOK    (WINAPI *PFN_SetWindowsHookExW)(int, HOOKPROC, HINSTANCE, DWORD);
typedef BOOL     (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
                                              LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                              LPVOID, LPCSTR, LPSTARTUPINFOA,
                                              LPPROCESS_INFORMATION);
typedef BOOL     (WINAPI *PFN_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                              LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                              LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                              LPPROCESS_INFORMATION);

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
static PFN_CreateProcessA      real_CreateProcessA;
static PFN_CreateProcessW      real_CreateProcessW;

/* 定义在文件后面 (注入自身/子进程) */
static void TryInjectOwner(DWORD pid);

/* 这些进程一律"挂起创建 -> 注入 -> 恢复":
   MultiClient 在收到命令后毫秒级就会 SwitchDesktop, 等 5 秒轮询注进去时
   桌面早就切走了。挂起注入保证 SwitchDesktop 的钩子在它第一次调用前就位。 */
static BOOL IsInjectAtBirth(const char* app, const char* cmd)
{
    static const char* names[] = {
        "MultiClient.exe", "ScreenRender.exe", "MultiRender.exe",
        "SharedDesktop.exe", "AudioRender.exe", NULL
    };
    char       base[64];
    char*      slash;
    const char *p, *c;
    int        i;

    base[0] = '\0';

    /* app 优先: 它是真实 exe 路径; cmd 可能被 "cmd /c ..." 之类包了一层。
       cmd 解析必须处理引号, 否则 "C:\Program Files\...\MultiClient.exe"
       会在 Program 后的空格处截断 —— 正式安装几乎都在 Program Files 下。 */
    if (app && app[0]) {
        _snprintf(base, sizeof(base) - 1, "%s", app);
        base[sizeof(base) - 1] = '\0';
    } else if (cmd && cmd[0]) {
        p = cmd;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '"') {
            p++;
            c = strchr(p, '"');
            if (!c) c = p + strlen(p);
        } else {
            c = p;
            while (*c && *c != ' ' && *c != '\t') c++;
        }
        if (c <= p) return FALSE;
        if ((size_t)(c - p) >= sizeof(base)) return FALSE;
        memcpy(base, p, (size_t)(c - p));
        base[c - p] = '\0';
    } else {
        return FALSE;
    }

    slash = strrchr(base, '\\');
    if (slash) memmove(base, slash + 1, strlen(slash + 1) + 1);

    for (i = 0; names[i]; i++)
        if (_stricmp(base, names[i]) == 0) return TRUE;
    return FALSE;
}


/* ------------------------------------------------------------------ */
/* 极简日志 (追加写, 失败就静默)                                        */
/* ------------------------------------------------------------------ */

static CRITICAL_SECTION g_logCs;
static int              g_logCsReady;
static HANDLE           g_logH = INVALID_HANDLE_VALUE;
static DWORD            g_logWrites;

static void HookLog(const char* fmt, ...)
{
    char    buf[512];
    char    line[640];
    char    fmtA[512];
    va_list ap;
    int     n;
    DWORD   wrote = 0;

    if (!g_logCsReady) return;

    /* 格式串是 UTF-8, %s 填的路径是 ACP: 统一转成 ACP 再格式化 */
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

    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        n = _snprintf(line, sizeof(line) - 1,
                      "[%02u:%02u:%02u.%03u] [pid %lu] %s\r\n",
                      (unsigned)st.wHour, (unsigned)st.wMinute,
                      (unsigned)st.wSecond, (unsigned)st.wMilliseconds,
                      (unsigned long)g_pid, buf);
    }
    if (n < 0) n = (int)strlen(line);
    line[sizeof(line) - 1] = '\0';
    if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;

    /* ★ 必须用 FILE_APPEND_DATA + 单次 WriteFile:
       这个日志是 3~4 个进程 (Student/MultiClient/ScreenRender) 同时写的。
       以前各进程用 fopen(...,"a") + fprintf — CRT 的 "a" 是"先 seek 到末尾再写",
       两个进程可能算出同一个末尾偏移, 于是互相覆盖: 日志里出现
       "fs=NTFS:29:14 [dll]..." 这种半行被吃掉、换行丢失的现象。
       FILE_APPEND_DATA 由文件系统保证追加到末尾, 不会再撞车。 */
    EnterCriticalSection(&g_logCs);
    /* 用户清日志时会直接删掉 bypass_hook.log。已打开的句柄会继续写进
       "已删除"的 inode, 于是文件再也不出现 —— 每 64 行检查一次, 没了就重开。 */
    if (g_logH != INVALID_HANDLE_VALUE && (++g_logWrites & 63) == 0 &&
        GetFileAttributesA(g_logPath) == INVALID_FILE_ATTRIBUTES) {
        CloseHandle(g_logH);
        g_logH = INVALID_HANDLE_VALUE;
    }
    if (g_logH == INVALID_HANDLE_VALUE) {
        g_logH = CreateFileA(g_logPath, FILE_APPEND_DATA,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (g_logH != INVALID_HANDLE_VALUE)
        WriteFile(g_logH, line, (DWORD)n, &wrote, NULL);
    LeaveCriticalSection(&g_logCs);
}

/* ------------------------------------------------------------------ */
/* 目标窗口判定                                                        */
/* ------------------------------------------------------------------ */

/* 进程名 (basename), 用于"这是不是同一个课堂软件的进程"的判断 */
static void BaseName(const char* path, char* out, int n)
{
    const char* p;
    if (!n) return;
    p = strrchr(path, '\\');
    p = p ? p + 1 : path;
    _snprintf(out, (size_t)n - 1, "%s", p);
    out[n - 1] = '\0';
}

static BOOL GetPidImage(DWORD pid, char* out, int n)
{
    HANDLE hp;
    char   full[MAX_PATH];
    DWORD  sz = sizeof(full);

    if (n > 0) out[0] = '\0';
    hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return FALSE;
    if (!QueryFullProcessImageNameA(hp, 0, full, &sz)) {
        CloseHandle(hp);
        return FALSE;
    }
    CloseHandle(hp);
    BaseName(full, out, n);
    return TRUE;
}

/* 教师端进程: 绝不接管。
   ★ 教师机上跑这套工具时, 教师端的 ScreenRender 是"广播预览窗", 不是学生端
     的广播窗; hook 进去只会把预览窗当小窗处理, 还会顺着 Teacher→ScreenSender
     扩散, 干扰真正的广播。这里做两道保险:
       1) IsClassroomExe 不再包含教师端 exe (不主动扩散);
       2) 万一被强行注入, Worker 在打补丁前直接退出。 */
static BOOL IsTeacherSideImage(const char* img)
{
    if (!img || !img[0]) return FALSE;
    return _stricmp(img, "Teacher.exe") == 0 ||
           _stricmp(img, "MultiTeach.exe") == 0 ||
           _stricmp(img, "ScreenSender.exe") == 0 ||
           _stricmp(img, "MediaFileSender.exe") == 0 ||
           _stricmp(img, "VideoTechConsole.exe") == 0;
}

/* 课堂软件的全部可执行文件名。投屏窗口可能由一个进程创建、由另一个进程
   摆放到全屏 (实测: 只拦"窗口属于本进程"的调用完全拦不住)。
   ⚠ 这里只列**学生端**进程: 教师端 (Teacher/ScreenSender/...) 由
     IsTeacherSideImage 明确排除, 否则教师机测试时会被误注入。 */
static BOOL IsClassroomExe(const char* name)
{
    static const char* t[] = {
        "Student.exe", "StudentMain.exe", "MultiClient.exe",
        "MmcStudent.exe",
        "MultiRender.exe", "ScreenRender.exe", "MultiScreen.exe",
        /* 与 inject32.exe 的目标列表对齐: 这些进程也可能创建/摆放广播窗口 */
        "AudioRender.exe", "AudioSender.exe", "SharedDesktop.exe",
        "StudentScreenHelper.exe",
        "ScreenPen.exe", "ScreenRecord.exe",
        NULL
    };
    int i;

    if (!name || !name[0]) return FALSE;
    if (IsTeacherSideImage(name)) return FALSE;
    for (i = 0; t[i]; i++)
        if (!_stricmp(name, t[i])) return TRUE;
    return FALSE;
}

/* pid -> "是不是课堂进程" 的小缓存, 避免每次钩子调用都 OpenProcess */
static BOOL IsClassroomPid(DWORD pid)
{
    static DWORD cachePid[16];
    static char  cacheRes[16];      /* 0 = 未知, 1 = 是, 2 = 否 */
    static volatile LONG next;
    int i;

    for (i = 0; i < 16; i++)
        if (cachePid[i] == pid && cacheRes[i]) return cacheRes[i] == 1;

    {
        char img[MAX_PATH];
        BOOL yes = GetPidImage(pid, img, sizeof(img)) && IsClassroomExe(img);
        LONG slot = InterlockedIncrement(&next) & 15;
        cachePid[slot] = pid;
        cacheRes[slot] = yes ? 1 : 2;
        return yes;
    }
}

/* 渲染器窗口类白名单 + 渲染器进程名。
   ★ Student.exe / MultiClient.exe 里也有自己的界面窗口 (实测
     StudentHelperWnd.oseasy: 学生的浮动工具条 / 虚拟桌面面板, 平时 350x70),
     它们和广播窗口同进程。只按尺寸判断会把它们也套成小窗, 再被
     "keep user geometry" 钉死 —— 用户看到的就是"软件自己把虚拟桌面变成小窗"。 */
static BOOL IsRenderWindowClass(const char* cls)
{
    if (!cls || !cls[0]) return FALSE;
    if (_strnicmp(cls, "MultiRender", 11) == 0) return TRUE;
    if (_strnicmp(cls, "StreamRenderWindow", 18) == 0) return TRUE;
    if (_strnicmp(cls, "OeScreenRenderWindow", 20) == 0) return TRUE;
    if (_stricmp(cls, "SDL_app") == 0) return TRUE;
    return FALSE;
}

static BOOL IsRendererImage(const char* img)
{
    if (!img || !img[0]) return FALSE;
    return _stricmp(img, "ScreenRender.exe") == 0 ||
           _stricmp(img, "MultiRender.exe") == 0 ||
           _stricmp(img, "SharedDesktop.exe") == 0;
}

/* 这个窗口是不是"广播画面"窗口 (只有它才归我们管)。
   ★ 进程里所有顶层窗口都会被我们子类化, 但真正允许改几何/样式的只有渲染器
     自己那个窗口; StudentHelperWnd 之类的界面窗口必须原样放过。 */
static BOOL IsBroadcastWindow(HWND h)
{
    char cls[96] = "";

    if (!h || !IsWindow(h)) return FALSE;
    GetClassNameA(h, cls, sizeof(cls));
    if (IsRenderWindowClass(cls)) return TRUE;
    return IsRendererImage(g_ownImage);
}

/* 本进程的父进程是不是某个镜像 (例如 MultiClient.exe)。
   用途: 渲染器拿不到 env/命令行标记时, 用"谁拉起的"来兜底判身份。 */
static BOOL ParentImageIs(const char* name)
{
    HANDLE snap;
    PROCESSENTRY32 pe;
    DWORD ppid = 0;
    BOOL  found = FALSE;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == g_pid) {
                ppid = pe.th32ParentProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }

    if (ppid) {
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                if (pe.th32ProcessID == ppid) {
                    found = (_stricmp(pe.szExeFile, name) == 0) ? TRUE : FALSE;
                    break;
                }
            } while (Process32Next(snap, &pe));
        }
    }

    CloseHandle(snap);
    return found;
}

/* 这个进程是不是"全屏广播"的渲染器?
   三条身份证据 —— 都不依赖窗口尺寸:
     1) BYPASS_BROADCAST=1 环境变量: 父进程被注入时, 我们把它的
        #fullscreen#:1 改写成 0 了 (参数里看不出来), 所以在环境里留了标记;
     2) 父进程没被注入时, 命令行里仍然是 #fullscreen#:1;
     3) 兜底: 镜像名是渲染器, 且父进程是 MultiClient.exe
        (只认 MultiClient 拉起的 —— 虚拟桌面预览那个 ScreenRender
         不是它拉起的, 不会被误伤)。
   三条都没有 = 不是广播渲染器, 这时才退回"窗口铺满 90% 屏"的几何兜底。 */
static int DetectBroadcastProc(void)
{
    char        env[8] = {0};
    const char* cmd = GetCommandLineA();
    const char* p;

    if (GetEnvironmentVariableA("BYPASS_BROADCAST", env, sizeof(env)) > 0 &&
        env[0] == '1') {
        g_forcedWindowMode = 1;      /* 是我们把 fullscreen:1 改成 0 的 */
        return 1;
    }

    if (cmd) {
        p = cmd;
        while ((p = strstr(p, "fullscreen")) != NULL) {
            const char* q = p + 10;
            /* 真实命令行是 {#decoderName#:#h264#,#fullscreen#:1,...} —— 中间隔着 # */
            while (*q == '#' || *q == ' ' || *q == '\t' || *q == '"' || *q == '\'') q++;
            if ((*q == ':' || *q == '=') && q[1] == '1' && q[2] != '0') {
                g_forcedWindowMode = 1;
                return 1;
            }
            p += 10;
        }
    }

    if (IsRendererImage(g_ownImage) && ParentImageIs("MultiClient.exe"))
        return 1;

    return 0;
}

/* 值得动手的窗口 = 真的满屏 (或没边框的大窗)。
   小弹窗 (输入法状态条、菜单、提示框) 一律不碰 —— 它们也有 WS_POPUP。
   ⚠ 带标题栏的普通窗口也不碰: 教室软件的「虚拟桌面」预览窗就是
   75% 屏 + 置顶 + 带框, 以前按 "2/3 屏" 判定会把它们也降级, 于是屏幕上
   出现两个小窗 (一个广播, 一个虚拟桌面)。 */
static BOOL IsWorthFixing(HWND h)
{
    RECT     r;
    int      w, hh;
    LONG_PTR st;
    char     cls[96] = "";
    int      sw = GetSystemMetrics(SM_CXSCREEN);
    int      sh = GetSystemMetrics(SM_CYSCREEN);

    /* 只认渲染器窗口: 学生端自己的界面 (StudentHelperWnd 等) 绝不碰 */
    GetClassNameA(h, cls, sizeof(cls));
    if (!IsRenderWindowClass(cls) && !IsRendererImage(g_ownImage)) return FALSE;

    if (!GetWindowRect(h, &r)) return FALSE;
    w  = r.right - r.left;
    hh = r.bottom - r.top;

    /* 已经小到"1/3 屏宽、半屏高"以内 —— 这就是我们自己设的小窗,
       别再动它 (否则每次兜底都会重设一次尺寸/样式, 白白闪一下)。 */
    if (w <= sw / 3 + 64 && hh <= sh / 2 + 64) return FALSE;

    /* ★ 身份明确 **并且是我们把 fullscreen:1 改成 0 的** -> 直接认, 不看尺寸。
       如果老师本来就发的是窗口模式 (fullscreen:0, 没有 BYPASS_BROADCAST 标记),
       渲染器一开始建的就是 3/4 屏的普通窗口 —— 那种不能碰, 要保持老师给的大小。 */
    if (g_isBroadcastProc && g_forcedWindowMode) return TRUE;

    /* 真的铺满整屏 (>= 90%) 才动手 */
    if (w >= sw * 9 / 10 && hh >= sh * 9 / 10) return TRUE;

    /* 有标题栏 = 已经是能拖能缩的普通窗口, 不需要我们改造 */
    st = GetWindowLongPtrW(h, GWL_STYLE);
    if (st & WS_CAPTION) return FALSE;

    if (w > g_maxW || hh > g_maxH) return TRUE;              /* 超过 2/3 屏 */
    if (w >= g_maxW / 2 && hh >= g_maxH / 2) return TRUE;    /* 至少半屏 */
    return FALSE;
}

/* 这个窗口看起来是不是"投屏/广播窗口" */
static BOOL IsBroadcastLike(HWND h)
{
    return IsWorthFixing(h);
}

static BOOL IsTargetWindow(HWND h)
{
    DWORD pid = 0;
    char  cls[64];
    int   n;

    if (!h || !IsWindow(h)) return FALSE;
    if (GetAncestor(h, GA_ROOT) != h) return FALSE;      /* 只处理顶层窗口 */

    GetWindowThreadProcessId(h, &pid);

    n = GetClassNameA(h, cls, sizeof(cls));
    if (n > 0) {
        if (!_stricmp(cls, "tooltips_class32")) return FALSE;
        if (!_stricmp(cls, "#32768"))            return FALSE;  /* 系统菜单 */
        if (!_strnicmp(cls, "IME", 3))           return FALSE;
        if (!_strnicmp(cls, "MSCTFIME", 8))      return FALSE;
        if (!_stricmp(cls, "ConsoleWindowClass")) return FALSE;
        /* ★ 系统影子/输入法/任务栏预览这些"外壳辅助窗口"绝不能被当成广播窗:
           它们属于渲染器进程, 但只是窗口边框的阴影/幽灵窗口, 一旦被子类化
           并套上几何规则, 会干扰真正的广播窗 (实测 SysShadow 被当成小窗管). */
        if (!_stricmp(cls, "SysShadow"))          return FALSE;
        if (!_stricmp(cls, "ForegroundStaging"))  return FALSE;
        if (!_stricmp(cls, "TaskListThumbnailWnd")) return FALSE;
        if (!_stricmp(cls, "MultitaskingViewFrame")) return FALSE;
        if (!_stricmp(cls, "WorkerW"))            return FALSE;
        if (!_stricmp(cls, "Shell_TrayWnd"))      return FALSE;
        if (!_stricmp(cls, "Progman"))            return FALSE;
    }

    if (pid == g_pid) return TRUE;

    /* --- 跨进程: 别的课堂进程 (Student/MultiClient/...) 在摆我们的窗口 ---
       实测证据: 注入成功的进程里每秒一次 "enforcer fixed 1 window(s)",
       却一条 "clamped" 都没有 => 改回全屏的那次调用不是窗口所有者自己发的。
       这里只对"看起来就是广播窗口"的外来窗口动手, 避免误伤别的进程的
       普通对话框。 */
    if (!IsClassroomPid(pid))  return FALSE;
    if (!IsBroadcastLike(h))   return FALSE;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 窗口子类化: 在渲染进程内部接管窗口过程                              */
/*                                                                    */
/* 为什么必须这么做 (实测):                                            */
/*   ScreenRender.exe 的 MultiRenderWindowClass 窗口, 我们去 SetWindowPos   */
/*   要 852x479, 1 秒后又回到 2558x1286, 而且全程没有任何 SetWindowPos  */
/*   调用进钩子 —— 因为窗口过程在 WM_GETMINMAXINFO 里把最小尺寸设成了   */
/*   整个桌面, 窗口管理器按这个下限把我们的请求夹回去了。                */
/*   样式改动 (去 POPUP/TOPMOST) 能保持住, 正是这个判断的旁证。          */
/*                                                                    */
/*   子类化之后:                                                       */
/*     WM_GETMINMAXINFO   -> 调完原过程后把 ptMinTrackSize 清零         */
/*     WM_WINDOWPOSCHANGING -> 调完原过程后把"全屏尺寸"改成小窗尺寸      */
/*   这是所有改变窗口几何的路径 (SetWindowPos / SetWindowPlacement /     */
/*   最大化 / 别的进程来改) 的唯一必经点, 比 IAT hook 更彻底。           */
/* ------------------------------------------------------------------ */

#define MAX_SUBCLASS 96

typedef struct {
    HWND    h;
    WNDPROC old;
} SUBENT;

static SUBENT           g_sub[MAX_SUBCLASS];
static CRITICAL_SECTION g_subCs;
static int              g_subCsReady;
static volatile LONG    g_subLogs;

static WNDPROC SubOldProc(HWND h)
{
    WNDPROC r;
    int i;

    /* 首选窗口属性: 每个窗口自带, 不受表容量限制, 也不会有锁竞争 */
    r = (WNDPROC)GetPropA(h, "BypassOldProc");
    if (r) return r;

    if (!g_subCsReady) return NULL;
    EnterCriticalSection(&g_subCs);
    for (i = 0; i < MAX_SUBCLASS; i++) {
        if (g_sub[i].h == h) { r = g_sub[i].old; break; }
    }
    LeaveCriticalSection(&g_subCs);
    return r;
}

static BOOL SubAdd(HWND h, WNDPROC old)
{
    SUBENT* slot = NULL;
    int i, found = 0;
    BOOL full = FALSE;

    if (!g_subCsReady) return FALSE;
    EnterCriticalSection(&g_subCs);
    for (i = 0; i < MAX_SUBCLASS; i++) {
        if (g_sub[i].h == h) { g_sub[i].old = old; found = 1; break; }
        if (!slot && (!g_sub[i].h || !IsWindow(g_sub[i].h))) slot = &g_sub[i];
    }
    if (found) {
        /* 已经登记过: 只更新原过程指针 (以前这里会误报 "table FULL") */
    } else if (slot) {
        slot->h = h;
        slot->old = old;
    } else {
        full = TRUE;
    }
    LeaveCriticalSection(&g_subCs);

    if (full) {
        /* 表满: 新窗口没法子类化 = 失去 WM_GETMINMAXINFO 保护, 必须留证据。
           ⚠ 日志必须放在释放 g_subCs 之后: HookLog 会拿 g_logCs 并做磁盘 IO,
             在锁里写日志会把窗口线程的 SubOldProc 和心跳线程一起拖住。 */
        if (InterlockedIncrement(&g_subLogs) <= 20)
            HookLog("!! subclass table FULL (%d), hwnd=%p not hooked",
                    MAX_SUBCLASS, (void*)h);
        return FALSE;
    }
    return TRUE;
}

/* ---- 用户交互状态: 拖边框/拖标题栏期间 (WM_ENTERSIZEMOVE..WM_EXITSIZEMOVE)
   窗口的几何完全由用户和系统决定, 我们一丝一毫都不能插手。
   这也是判断"这次几何变化是不是用户干的"的唯一可靠信号:
   用户拖动时系统直接改窗口, 不经过应用的 SetWindowPos/MoveWindow。 ---- */
#define PROP_SIZING  "BypassSizing"
#define PROP_ZOOMED  "BypassZoomed"
#define PROP_USERCMD "BypassUserCmd"     /* 用户刚下过命令时的 GetTickCount */
#define PROP_USERMAX "BypassUserMax"     /* 用户主动(模拟)最大化: enforcer 不许再降级 */
#define PROP_ICONIC  "BypassIconic"      /* 窗口正处于最小化状态 (用于"恢复时保持最大化") */
#define PROP_PARKED  "BypassParked"      /* 用户点最小化 -> 窗口已隐藏 (关掉持续保持小窗后渲染器会重新显示) */
#define PROP_TASKBARMIN "BypassTaskbarMin" /* 真最小化到任务栏期间: 应用任何"显示/还原"都吃掉 */
#define PROP_CLICKACTIVE "BypassClickActive" /* 最近一次"点击激活"(WA_CLICKACTIVE) 的 tick */
#define PROP_MENUTICK  "BypassMenuTick"   /* 最近一次窗口被激活/打开系统菜单的 tick */
#define PROP_CAPMAX    "BypassCapMaxClick" /* 最近一次点了标题栏"最大化"按钮的 tick */
#define PROP_RESTORED  "BypassRestoredTick" /* 最近一次从最小化被恢复的 tick */
#define PROP_CAPHIDE  "BypassCapHide"    /* 标题栏/控制按钮已自动隐藏 (摘掉了 WS_CAPTION) */
#define PROP_CAPHOVER "BypassCapHover"   /* 鼠标最后一次停在标题栏里的时刻 */
#define PROP_CAPTIMER "BypassCapTimer"   /* 这个窗口已装过自动隐藏定时器 */
#define WM_BYPASS_FIX   (WM_APP + 0x51)
#define WM_BYPASS_CAP   (WM_APP + 0x52)  /* 1=显示标题栏, 0=隐藏, 2=立即判定一次 */
#define WM_BYPASS_UNPARK (WM_APP + 0x53) /* 恢复被"收起"(隐藏)的广播窗 */
#define WM_BYPASS_REMIN  (WM_APP + 0x54) /* 任务栏最小化窗口又被顶出来 -> 压回去 */
#define TIMER_CAPTION   0xB1C0           /* 自动隐藏标题栏轮询 (只在窗口线程) */
#define CAPTION_REVEAL_PX  8             /* 隐藏后鼠标进入顶边多少像素就显示 */
#define CAPTION_HIDE_MS    1200          /* 鼠标离开标题栏后多久自动隐藏 */

static BOOL InSizeMove(HWND h)
{
    return GetPropA(h, PROP_SIZING) != NULL;
}

/* 最近 ms 毫秒内, 用户对窗口下过"用户级命令"吗?
   (点还原/最大化按钮、双击标题栏、拖动、键盘 Win+Up/Down ...)
   ★ 这是区分"用户在还原"和"渲染器在偷偷还原"的唯一可靠信号:
     前者走 WM_SYSCOMMAND / WM_NCLBUTTONDBLCLK, 后者只是应用自己调
     ShowWindow(SW_RESTORE) —— 实测渲染器每 500ms 就来一次, 不区分的话
     用户点了最大化会在半秒内被它自己退掉。 */
static BOOL UserCmdRecent(HWND h, DWORD ms)
{
    DWORD t = (DWORD)(ULONG_PTR)GetPropA(h, PROP_USERCMD);

    if (!t) return FALSE;
    return (GetTickCount() - t) <= ms;
}

/* 这次"恢复"是不是用户干的?
   应用自己每 100~300ms 就在调 ShowWindow(SW_SHOW)/SC_RESTORE 把窗口顶回来,
   而用户真正恢复只有两三种动作:
     - 点任务栏按钮 / 缩略图 / 右键菜单"还原"
     - Win / Alt+Tab
     - 直接点窗口 (如果它已经被顶出来一瞬)
   这些动作都有共同的"输入"特征: 光标在任务栏(或菜单)上且刚刚有输入, 或者
   窗口刚收到 WA_CLICKACTIVE, 或者按着 Win/Alt 键。应用自己发消息时没有这些特征。
   ★ 判错了也不要紧: 关掉「持续保持小窗」(写 bypass_hook.off) 后 DLL 完全放行,
     渲染器下一次 ShowWindow 会把窗口重新显示出来。 */
static BOOL UserRestoreSignal(HWND h, DWORD withinMs)
{
    DWORD now = GetTickCount();
    DWORD t   = (DWORD)(ULONG_PTR)GetPropA(h, PROP_CLICKACTIVE);
    DWORD m   = (DWORD)(ULONG_PTR)GetPropA(h, PROP_MENUTICK);

    if (t && (DWORD)(now - t) <= withinMs) return TRUE;

    /* 系统菜单/右键菜单/Alt+空格: 应用不会自己打开窗口菜单, 所以
       "菜单刚开过" + 接下来的 SC_RESTORE/SC_MAXIMIZE 一定是用户点的
       (任务栏右键 -> 还原/最大化 走的就是这条路)。 */
    if (m && (DWORD)(now - m) <= withinMs) return TRUE;

    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) ||
        (GetAsyncKeyState(VK_MENU) & 0x8000))
        return TRUE;

    {
        POINT pt;
        HWND  w;
        if (GetCursorPos(&pt)) {
            BOOL nearTray = FALSE;
            w = WindowFromPoint(pt);
            if (w) {
                char cls[96] = "";
                GetClassNameA(w, cls, sizeof(cls));
                if (strstr(cls, "Shell_TrayWnd") || strstr(cls, "TrayNotifyWnd") ||
                    strstr(cls, "TaskList")      || strstr(cls, "MSTask") ||
                    strstr(cls, "Notify")        || strstr(cls, "#32768") ||
                    strstr(cls, "DV2ControlHost") ||
                    /* Win10/11 的任务栏和缩略图预览是 XAML 窗口, 类名不再是
                       Shell_TrayWnd —— 不认它们的话, "点预览图"会被当成应用
                       消息拦掉。 */
                    strstr(cls, "CoreWindow")    || strstr(cls, "Windows.UI") ||
                    strstr(cls, "Taskbar")       || strstr(cls, "XamlExplorer"))
                    nearTray = TRUE;
            }
            if (!nearTray) {
                /* 几何兜底: 光标在任务栏矩形(向上扩 240px, 覆盖缩略图预览)内。
                   只用于"最小化期间判断这是不是用户在点任务栏", 不会影响别的逻辑。 */
                HWND tray = FindWindowA("Shell_TrayWnd", NULL);
                RECT tr;
                if (tray && GetWindowRect(tray, &tr)) {
                    tr.top -= 240;
                    tr.bottom += 8;
                    if (pt.x >= tr.left && pt.x < tr.right &&
                        pt.y >= tr.top  && pt.y < tr.bottom)
                        nearTray = TRUE;
                }
            }
            if (nearTray) {
                LASTINPUTINFO li;
                li.cbSize = sizeof(li);
                if (GetLastInputInfo(&li) &&
                    (DWORD)(now - li.dwTime) <= withinMs)
                    return TRUE;
            }
        }
    }
    return FALSE;
}

static BOOL TaskbarMinimized(HWND h)
{
    return GetPropA(h, PROP_TASKBARMIN) != NULL;
}

/* 窗口是不是还停在标准最小化位置 (-32000,-32000)。
   ★ 有这种情况: 恢复命令到了、窗口 de-iconify 了, 但几何被冻结/被打断,
     矩形还留在 -32000 —— 用户看就是"点了任务栏图标但没出来"。 */
static BOOL AtIconicRect(HWND h)
{
    RECT r;
    if (!GetWindowRect(h, &r)) return FALSE;
    return (r.left <= -30000 || r.top <= -30000) ? TRUE : FALSE;
}

/* 用户真的把"最小化到任务栏"的窗口叫回来了: 解除锁, 记一次用户命令。 */
static void ReleaseTaskbarMin(HWND h, const char* why)
{
    RemovePropA(h, PROP_TASKBARMIN);
    RemovePropA(h, PROP_CLICKACTIVE);
    SetPropA(h, PROP_RESTORED, (HANDLE)(ULONG_PTR)GetTickCount());
    SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());
    HookLog("taskbar minimize released hwnd=%p (%s)", (void*)h, why);
}

static LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
static BOOL EnsureSubclassed(HWND h)
{
    DWORD   pid = 0;
    WNDPROC old;
    char    cls[64];
    int     n;

    if (!g_subCsReady || !h || !IsWindow(h)) return FALSE;

    GetWindowThreadProcessId(h, &pid);
    if (pid != g_pid) return FALSE;          /* 只能改自己进程的窗口过程 */
    if (SubOldProc(h)) return TRUE;          /* 已装过 */

    /* ★ 只接管"广播画面"窗口。进程里其它顶层窗口 (StudentHelperWnd 工具条、
       虚拟桌面面板、提示框) 保持原样 —— 以前把它们也子类化, 再叠上
       "满屏就夹成小窗"的逻辑, 结果学生端自己的界面也被套成了小窗。 */
    if (!IsBroadcastWindow(h)) return FALSE;

    n = GetClassNameA(h, cls, sizeof(cls));
    if (n > 0) {
        if (!_stricmp(cls, "tooltips_class32")) return FALSE;
        if (!_stricmp(cls, "#32768"))            return FALSE;
        if (!_strnicmp(cls, "IME", 3))           return FALSE;
        if (!_strnicmp(cls, "MSCTFIME", 8))      return FALSE;
        if (!_stricmp(cls, "ConsoleWindowClass")) return FALSE;
        if (!_stricmp(cls, "SysShadow"))          return FALSE;
        if (!_stricmp(cls, "ForegroundStaging"))  return FALSE;
        if (!_stricmp(cls, "TaskListThumbnailWnd")) return FALSE;
        if (!_stricmp(cls, "MultitaskingViewFrame")) return FALSE;
        if (!_stricmp(cls, "WorkerW"))            return FALSE;
    }

    old = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubclassProc);
    if (!old || old == SubclassProc) return FALSE;

    /* 原过程指针挂在窗口属性上 —— 这是主存储, 表只是兼容/诊断用途。
       这样即使表满了, 这个窗口仍然能正常回调原过程。 */
    SetPropA(h, "BypassOldProc", (HANDLE)old);
    SubAdd(h, old);

    if (InterlockedIncrement(&g_subLogs) <= 20)
        HookLog("subclassed hwnd=%p cls=%s", (void*)h, (cls[0] ? cls : "?"));
    return TRUE;
}

/* 这个矩形是不是"铺满当前显示器工作区"(尺寸近似即可) */
static BOOL IsWorkAreaSized(HWND h, const RECT* r)
{
    HMONITOR    mon;
    MONITORINFO mi;
    int aw, ah;

    if (!r) return FALSE;
    mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;

    aw = mi.rcWork.right - mi.rcWork.left;
    ah = mi.rcWork.bottom - mi.rcWork.top;
    return (abs((int)(r->right - r->left) - aw) <= 16 &&
            abs((int)(r->bottom - r->top) - ah) <= 16);
}

/* 这个矩形是不是"铺满整个显示器"(外框被推到屏幕外, 真实最大化的样子)。
   我们没有 WS_MAXIMIZE, 最大化后的外框会比显示器大一圈, 必须单独认。 */
static BOOL IsMonitorSized(HWND h, const RECT* r)
{
    HMONITOR    mon;
    MONITORINFO mi;
    int aw, ah;

    if (!r) return FALSE;
    mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return FALSE;

    aw = mi.rcMonitor.right - mi.rcMonitor.left;
    ah = mi.rcMonitor.bottom - mi.rcMonitor.top;
    return (abs((int)(r->right - r->left) - aw) <= 24 &&
            abs((int)(r->bottom - r->top) - ah) <= 24);
}

/* "模拟最大化"的目标矩形: 和系统最大化的几何一致 —— 外框比显示器大一圈,
   边框推到屏幕外, 这样客户区正好铺满整个显示器 (不会留边框缝隙)。
   客户区 = 显示器 = 隐藏标题栏后的画面。 */
static void MaxRectFor(HWND h, RECT* out)
{
    HMONITOR    mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi;
    int fx, fy;

    if (!out) return;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) {
        /* 拿不到显示器信息时退回当前窗口矩形, 绝不让调用方用到未初始化的 RECT */
        if (!GetWindowRect(h, out)) {
            out->left = 0;
            out->top = 0;
            out->right = GetSystemMetrics(SM_CXSCREEN);
            out->bottom = GetSystemMetrics(SM_CYSCREEN);
        }
        return;
    }

    fx = GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    fy = GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;

    out->left   = mi.rcMonitor.left - fx;
    out->top    = mi.rcMonitor.top  - fy;
    out->right  = mi.rcMonitor.right  + fx;
    out->bottom = mi.rcMonitor.bottom + fy;
}

/* ---- 标题栏（最小化 / 最大化 / 关闭那一块）自动隐藏 ----
 *
 * 行为:
 *   - 只对"用户把广播小窗最大化"之后的窗口生效 (BypassUserMax);
 *   - 隐藏 = 摘掉 WS_CAPTION: 标题栏连同三个按钮一起消失, 画面看起来像全屏;
 *   - 鼠标移到窗口顶边 (CAPTION_REVEAL_PX 像素内) -> 控制栏立刻回来;
 *   - 鼠标离开标题栏 CAPTION_HIDE_MS 毫秒后再次隐藏。
 *
 * 为什么用摘样式而不是画假标题栏:
 *   渲染器的客户区是 SDL 直接渲染的, 自绘非客户区会和它抢 WM_NCPAINT;
 *   摘掉 WS_CAPTION 让系统自己把客户区铺满, 最稳, 也不会留下残影。
 *
 * ★ 所有 SetWindowLong/SetWindowPos 都只在"窗口自己的线程"里调用:
 *   本函数只从子类过程 / 它的 WM_TIMER 进入; 别的线程只发 WM_BYPASS_CAP。 */
static BOOL CaptionAutoHideOn(void)
{
    return g_stTitle != 0;
}

/* 最小化行为: 默认真最小化到任务栏; 配置 minimize_mode=tray 则改为"收起"。
   内联 hook ShowWindow 之后, 渲染器再想用 SW_SHOW 顶回来会被拦掉。 */
static BOOL MinimizeToTaskbarMode(void)
{
    return g_stMinTray == 0;
}

static void CaptionApply(HWND h, BOOL show)
{
    LONG_PTR st;
    RECT     r;

    if (!IsWindow(h)) return;
    st = GetWindowLongPtrW(h, GWL_STYLE);

    if (show) {
        /* 先清标记, 再改样式: WM_STYLECHANGED 里会看这个标记决定要不要补外框 */
        RemovePropA(h, PROP_CAPHIDE);
        if (!(st & WS_CAPTION)) {
            st |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            st &= ~(LONG_PTR)WS_POPUP;
            /* ★ 用 SWP_NOSENDCHANGING: 这次几何/边框变更不经过
               WM_WINDOWPOSCHANGING, 免得下面的"冻结应用几何"把自己也挡了;
               而渲染器在 WM_SIZE 里紧接着发的 SetWindowPos 没有这个标志,
               会被照常冻住, 不会把我们摆好的满屏位置改回带缝隙的样子。 */
            real_SetWindowLongW(h, GWL_STYLE, (LONG)st);
            real_SetWindowPos(h, NULL, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                              SWP_NOACTIVATE | SWP_FRAMECHANGED |
                              SWP_NOSENDCHANGING);
            SetPropA(h, PROP_CAPHOVER, (HANDLE)(ULONG_PTR)GetTickCount());
            if (InterlockedIncrement(&g_capLogs) <= 20)
                HookLog("caption auto-show hwnd=%p (control bar visible)", (void*)h);
        }
        return;
    }

    /* 只隐藏"用户模拟最大化"的窗口; 普通小窗必须保留标题栏给用户拖/缩放 */
    if (!GetPropA(h, PROP_USERMAX)) return;
    if (!(st & WS_CAPTION)) return;

    SetPropA(h, PROP_CAPHIDE, (HANDLE)1);
    st &= ~(LONG_PTR)(WS_CAPTION | WS_POPUP);
    real_SetWindowLongW(h, GWL_STYLE, (LONG)st);
    real_SetWindowPos(h, NULL, 0, 0, 0, 0,
                      SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                      SWP_NOACTIVATE | SWP_FRAMECHANGED |
                      SWP_NOSENDCHANGING);

    if (GetWindowRect(h, &r) && InterlockedIncrement(&g_capLogs) <= 20)
        HookLog("caption auto-hide hwnd=%p %dx%d@(%d,%d) (maximized broadcast)",
                (void*)h, (int)(r.right - r.left), (int)(r.bottom - r.top),
                (int)r.left, (int)r.top);
}

/* 由窗口线程的定时器 (150ms) 以及鼠标移动消息随时调用 */
static void CaptionTick(HWND h)
{
    RECT    r;
    POINT   pt;
    LONG_PTR st;
    DWORD   now, last;
    int     capH, bandTop, bandL, bandR;
    HMONITOR    mon;
    MONITORINFO mi;

    if (!IsWindow(h)) return;

    /* 关闭开关 / 窗口不归我们管 / 已经还原 -> 保证标题栏在 */
    if (g_disabled || !CaptionAutoHideOn() || !IsManaged(h) ||
        IsIconic(h) || !IsWindowVisible(h)) {
        if (GetPropA(h, PROP_USERMAX) &&
            InterlockedIncrement(&g_capTraceLogs) <= 10)
            HookLog("caption tick skip hwnd=%p (disabled=%d autohide=%d managed=%d "
                    "iconic=%d visible=%d)",
                    (void*)h, (int)g_disabled, (int)CaptionAutoHideOn(),
                    (int)IsManaged(h), (int)IsIconic(h), (int)IsWindowVisible(h));
        if (GetPropA(h, PROP_CAPHIDE) && !IsIconic(h)) CaptionApply(h, TRUE);
        return;
    }

    /* 没最大化: 标题栏必须一直可见 (否则小窗拖不动、按钮也点不到) */
    if (!GetPropA(h, PROP_USERMAX)) {
        if (GetPropA(h, PROP_CAPHIDE)) CaptionApply(h, TRUE);
        return;
    }

    /* 用户正在拖标题栏/拉边框: 不要在这期间隐藏/恢复标题栏,
       否则 FRAMECHANGED 会打断系统的移动/缩放循环 (表现为"拖不动") */
    if (InSizeMove(h)) return;

    if (!GetWindowRect(h, &r)) return;
    /* ★ 刚从最小化恢复的一瞬间, 窗口还停在 (-32000,-32000) 的标准最小化
       位置; 这时算出来的标题栏判定带是无效的 (bandR < bandL), 不要在这时
       隐藏/恢复标题栏 —— 否则标题栏会在窗口还看不见的时候被摘掉。 */
    if (r.left <= -30000 || r.top <= -30000) return;
    if (!GetCursorPos(&pt)) return;

    /* ★ 隐藏标题栏后窗口外框在屏幕外 (top=-8), 直接用 r.top 当"顶边"
       会得到一个屏幕外的判定带 [−9,−2], 鼠标永远够不到 —— 这就是
       "隐藏以后叫不回来"的原因。唤出带必须以**显示器上沿**为准。 */
    mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return;

    bandTop = (r.top   > mi.rcMonitor.top)    ? r.top   : mi.rcMonitor.top;
    bandL   = (r.left  > mi.rcMonitor.left)   ? r.left  : mi.rcMonitor.left;
    bandR   = (r.right < mi.rcMonitor.right)  ? r.right : mi.rcMonitor.right;

    now = GetTickCount();
    st = GetWindowLongPtrW(h, GWL_STYLE);

    capH = GetSystemMetrics(SM_CYCAPTION) +
           GetSystemMetrics(SM_CYSIZEFRAME) * 2;
    if (capH < 16) capH = 16;

    /* 诊断: 每秒最多一行。用户最大化后如果一直不隐藏, 看这行就能判断
       是"tick 没来"、"光标一直停在标题栏里", 还是"开关文件被建了"。 */
    if (GetPropA(h, PROP_USERMAX) && (now - g_capTraceTick) >= 1000 &&
        InterlockedIncrement(&g_capTraceLogs) <= 200) {
        DWORD idle = now - (DWORD)(ULONG_PTR)GetPropA(h, PROP_CAPHOVER);
        g_capTraceTick = now;
        HookLog("caption tick hwnd=%p pt=(%d,%d) band=(%d,%d)-(%d,%d) capH=%d "
                "caption=%d idle=%lums",
                (void*)h, (int)pt.x, (int)pt.y, bandL, bandTop, bandR,
                bandTop + capH, capH, (st & WS_CAPTION) ? 1 : 0,
                (unsigned long)idle);
    }

    if (!(st & WS_CAPTION)) {
        /* 隐藏中: 鼠标到"屏幕顶边"就显示控制栏 */
        if (pt.x >= bandL && pt.x < bandR &&
            pt.y >= bandTop - 2 && pt.y <= bandTop + CAPTION_REVEAL_PX) {
            SetPropA(h, PROP_CAPHOVER, (HANDLE)(ULONG_PTR)now);
            CaptionApply(h, TRUE);
        }
        return;
    }

    /* 显示中: 鼠标在标题栏里就续期; 离开一会儿自动隐藏 */
    if (pt.x >= bandL && pt.x < bandR &&
        pt.y >= bandTop && pt.y < bandTop + capH) {
        SetPropA(h, PROP_CAPHOVER, (HANDLE)(ULONG_PTR)now);
    } else {
        last = (DWORD)(ULONG_PTR)GetPropA(h, PROP_CAPHOVER);
        if (!last) {
            SetPropA(h, PROP_CAPHOVER, (HANDLE)(ULONG_PTR)now);
        } else if ((DWORD)(now - last) >= CAPTION_HIDE_MS) {
            HookLog("caption hide due hwnd=%p (idle=%lums, pt=%d,%d)",
                    (void*)h, (unsigned long)(now - last), (int)pt.x, (int)pt.y);
            CaptionApply(h, FALSE);
        }
    }
}

/* 用户显式要求恢复 (任务栏按钮/缩略图/SC_RESTORE/主程序「显示」按钮)。
   ★ 必须由我们自己把几何摆回去, 不能只靠 DefWindowProc:
     `SC_RESTORE` 内部的 SetWindowPos 会被下面"几何归用户"的冻结拦掉,
     结果窗口 de-iconify 了、矩形却还停在 (-32000,-32000) ——
     用户看到的就是"点了任务栏图标但窗口没出来"。 */
static void RestoreManagedWindow(HWND h, const char* why)
{
    RECT ur;
    BOOL wasIconic  = IsIconic(h) ? TRUE : FALSE;
    BOOL wasParked  = GetPropA(h, PROP_PARKED) ? TRUE : FALSE;
    BOOL wasTaskbar = TaskbarMinimized(h) ? TRUE : FALSE;

    RemovePropA(h, PROP_PARKED);
    if (wasTaskbar) ReleaseTaskbarMin(h, why);
    SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());

    if (real_ShowWindow)
        real_ShowWindow(h, wasIconic ? SW_RESTORE : SW_SHOW);
    CaptionApply(h, TRUE);                 /* 控制栏一起回来 */

    if (GetPropA(h, PROP_USERMAX)) {
        RECT mr;
        MaxRectFor(h, &mr);
        real_SetWindowPos(h, NULL, mr.left, mr.top,
                          mr.right - mr.left, mr.bottom - mr.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    } else if (GetUserRect(h, &ur)) {
        real_SetWindowPos(h, NULL, ur.left, ur.top,
                          ur.right - ur.left, ur.bottom - ur.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    } else {
        /* 没有记录过用户矩形: 用默认小窗位置, 至少保证在屏幕内可见 */
        real_SetWindowPos(h, NULL, g_smallX, g_smallY, g_smallW, g_smallH,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    }

    HookLog("restored by request hwnd=%p (%s; iconic=%d parked=%d taskbar=%d)",
            (void*)h, why, (int)wasIconic, (int)wasParked, (int)wasTaskbar);
}

/* 模拟最大化 / 还原。
   ★ 不用 ShowWindow(SW_MAXIMIZE): 一旦置上 WS_MAXIMIZE, 非客户区按最大化
     布局, 但我们又冻结几何, 命中测试就会错位 (按钮点不动)。
   这里用普通窗口铺满**整个显示器**(外框推到屏幕外), 并打 BypassUserMax 标记:
     - enforcer / 主程序 watcher 都不再降级它;
     - 用户拖动它时, 只要尺寸还是满屏大小, 标记和"还原点"都保留,
       所以不会"一拖就变回小窗"。
   为什么用 rcMonitor 而不是 rcWork: 用 rcWork 时窗口外框正好贴工作区,
   WS_THICKFRAME 的边框会占掉几条像素, 画面四周就留出"缝隙"。按真实最大化的
   做法把外框推到屏幕外, 客户区才正好铺满整屏。 */
static void ToggleEmulatedMax(HWND h)
{
    RECT cur, ur, mr;
    LONG_PTR st;

    if (!GetWindowRect(h, &cur)) return;
    /* 已经是"满屏"(模拟最大化 / 系统最大化 / 铺满工作区) -> 还原到用户矩形 */
    if (GetPropA(h, PROP_USERMAX) ||
        IsWorkAreaSized(h, &cur) || IsMonitorSized(h, &cur)) {
        if (GetUserRect(h, &ur) && (ur.right - ur.left) > 120) {
            RemovePropA(h, PROP_USERMAX);
            CaptionApply(h, TRUE);          /* 还原后必须把标题栏/按钮还回来 */
            st = GetWindowLongPtrW(h, GWL_STYLE);
            if (st & WS_MAXIMIZE)           /* 系统最大化过 -> 先摘掉状态位 */
                real_SetWindowLongW(h, GWL_STYLE, (LONG)(st & ~WS_MAXIMIZE));
            SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());
            real_SetWindowPos(h, NULL, ur.left, ur.top,
                              ur.right - ur.left, ur.bottom - ur.top,
                              SWP_NOZORDER | SWP_NOACTIVATE |
                              SWP_NOSENDCHANGING);
            HookLog("user restore hwnd=%p -> %dx%d@(%d,%d) (emulated)",
                    (void*)h, (int)(ur.right - ur.left),
                    (int)(ur.bottom - ur.top), (int)ur.left, (int)ur.top);
        }
        return;
    }

    /* 记住当前小窗位置, 再铺满整个显示器 (外框推到屏幕外, 无边框缝隙) */
    SetUserRect(h, &cur);
    SetPropA(h, PROP_USERMAX, (HANDLE)1);
    SetPropA(h, PROP_CAPHOVER, (HANDLE)(ULONG_PTR)GetTickCount());
    st = GetWindowLongPtrW(h, GWL_STYLE);
    if (st & WS_MAXIMIZE)
        real_SetWindowLongW(h, GWL_STYLE, (LONG)(st & ~WS_MAXIMIZE));

    mr = cur;                                /* 取显示器信息失败时退回原矩形 */
    MaxRectFor(h, &mr);
    SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());
    real_SetWindowPos(h, NULL, mr.left, mr.top,
                      mr.right - mr.left, mr.bottom - mr.top,
                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    HookLog("user maximize hwnd=%p -> monitor %dx%d@(%d,%d) (emulated, borders off-screen)",
            (void*)h, (int)(mr.right - mr.left), (int)(mr.bottom - mr.top),
            (int)mr.left, (int)mr.top);
}

/* 最小化状态跳变 (用户从任务栏恢复等)。
   最小化按钮已禁用 (见 SC_MINIMIZE 处理); 若窗口仍被系统最小化后又恢复,
   保持"模拟最大化"并补回标题栏。不再和渲染器抢最小化状态。 */
static void HandleIconicTransition(HWND h, BOOL iconNow)
{
    BOOL iconWas = GetPropA(h, PROP_ICONIC) != NULL;

    if (iconNow) {
        if (!iconWas) SetPropA(h, PROP_ICONIC, (HANDLE)1);
        return;
    }
    if (!iconWas) return;

    RemovePropA(h, PROP_ICONIC);
    if (!GetPropA(h, PROP_CAPHIDE)) CaptionApply(h, TRUE);
    if (GetPropA(h, PROP_USERMAX)) {
        RECT mr;
        MaxRectFor(h, &mr);
        real_SetWindowPos(h, NULL, mr.left, mr.top,
                          mr.right - mr.left, mr.bottom - mr.top,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        HookLog("restored from minimize -> kept maximized hwnd=%p %dx%d@(%d,%d)",
                (void*)h, (int)(mr.right - mr.left), (int)(mr.bottom - mr.top),
                (int)mr.left, (int)mr.top);
    }
}

static LRESULT CALLBACK SubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    WNDPROC old = SubOldProc(h);
    LRESULT r;

    /* 标题栏自动隐藏: 定时器装在窗口自己的线程里。
       (跨线程 SetTimer 的 WM_TIMER 会投到别的线程, 再跨线程改样式会互等。) */
    if (!GetPropA(h, PROP_CAPTIMER)) {
        SetPropA(h, PROP_CAPTIMER, (HANDLE)1);
        if (SetTimer(h, TIMER_CAPTION, 150, NULL))
            HookLog("caption timer armed hwnd=%p (150ms)", (void*)h);
        else if (InterlockedIncrement(&g_capCheckLogs) <= 5)
            HookLog("WARNING: caption timer failed hwnd=%p err=%lu",
                    (void*)h, GetLastError());
    }
    if (msg == WM_TIMER && wp == (WPARAM)TIMER_CAPTION) {
        if (!GetPropA(h, "BypassCapTickSeen")) {
            SetPropA(h, "BypassCapTickSeen", (HANDLE)1);
            HookLog("caption timer first tick hwnd=%p", (void*)h);
        }
        CaptionTick(h);
        return 0;
    }
    if (msg == WM_BYPASS_CAP) {
        if (wp == 2) CaptionTick(h);
        else         CaptionApply(h, wp ? TRUE : FALSE);
        return 0;
    }
    if (msg == WM_BYPASS_REMIN) {
        /* 由心跳线程发现"锁还在、窗口却已经不是最小化"后投递到窗口线程。
           用户真的恢复了就解锁, 否则立刻压回任务栏。 */
        if (!g_disabled && TaskbarMinimized(h) && !IsIconic(h)) {
            if (UserRestoreSignal(h, 1500)) {
                ReleaseTaskbarMin(h, "pump saw restored");
                PostMessageA(h, WM_BYPASS_UNPARK, 0, 0);   /* 确认真的摆回屏幕内 */
            } else if (real_ShowWindow) {
                if (InterlockedIncrement(&g_minLogs) <= 20)
                    HookLog("re-minimize hwnd=%p (pump saw restored without user)",
                            (void*)h);
                real_ShowWindow(h, SW_MINIMIZE);
            }
        }
        return 0;
    }
    if (msg == WM_BYPASS_UNPARK) {
        /* 主程序的「持续保持小窗」恢复路径仍会投递这条消息。
           不论是"收起"(PARKED)、"任务栏最小化"(TASKBARMIN), 还是锁已经意外
           丢了但窗口仍然是 iconic / 还在 -32000 的兜底状态, 都强制恢复一次。 */
        if (GetPropA(h, PROP_PARKED) || TaskbarMinimized(h) ||
            (IsManaged(h) && (IsIconic(h) || AtIconicRect(h)))) {
            RestoreManagedWindow(h, "WM_BYPASS_UNPARK");
        }
        return 0;
    }

    /* ★ 鼠标一动就立刻判一次: 光靠 150ms 定时器太钝, 用户常常"刷"一下就
       划到屏幕顶边, 那一下没落在定时器的采样里, 控制栏就"叫不回来"。
       这里只是让唤出更跟手, 处理本身很轻 (命中判定 + 状态跳变才动窗口)。 */
    if (!g_disabled &&
        (msg == WM_MOUSEMOVE   || msg == WM_NCMOUSEMOVE ||
         msg == WM_LBUTTONDOWN || msg == WM_NCLBUTTONDOWN ||
         msg == WM_NCLBUTTONDBLCLK)) {
        CaptionTick(h);
    }

    /* 点击激活 (任务栏按钮 / 点窗口) 是最可靠的"用户在操作"信号:
       应用自己 ShowWindow(SW_SHOW) 只会产生 WA_ACTIVE, 不会是 WA_CLICKACTIVE。
       如果这时窗口已经不在最小化状态, 说明系统已经把它恢复了(这种系统恢复
       不一定发 WM_WINDOWPOSCHANGED), 顺手把任务栏最小化锁解掉 —— 否则用户
       接下来点最大化按钮会被当成"应用发来的 SC_MAXIMIZE"而拦截。 */
    if (msg == WM_ACTIVATE && LOWORD((DWORD_PTR)wp) == WA_CLICKACTIVE) {
        SetPropA(h, PROP_CLICKACTIVE, (HANDLE)(ULONG_PTR)GetTickCount());
        if (TaskbarMinimized(h)) {
            if (IsIconic(h))
                PostMessageA(h, WM_BYPASS_UNPARK, 0, 0);
            else {
                ReleaseTaskbarMin(h, "WA_CLICKACTIVE");
                PostMessageA(h, WM_BYPASS_UNPARK, 0, 0);
            }
        }
    }

    /* 最小化期间收到的鼠标激活/点击: 任务栏缩略图预览会把点击转发给窗口
       (Win10 的预览点击甚至只发 WM_MOUSEACTIVATE/LBUTTONDOWN) —— 都当成
       "用户要恢复"。窗口已经可见的话, 只解锁 (可能还要修正 -32000 半状态)。 */
    if ((msg == WM_MOUSEACTIVATE || msg == WM_LBUTTONDOWN ||
         msg == WM_NCLBUTTONDOWN) && TaskbarMinimized(h)) {
        if (IsIconic(h))
            PostMessageA(h, WM_BYPASS_UNPARK, 0, 0);
        else {
            ReleaseTaskbarMin(h, "click while taskbar-minimized");
            PostMessageA(h, WM_BYPASS_UNPARK, 0, 0);
        }
    }

    /* 记录"用户确实点了最大化按钮"。DefWindowProc 随后才会发 SC_MAXIMIZE,
       应用自己发的 SC_MAXIMIZE 没有这个前置事件 —— 用它区分用户和应用。 */
    if (msg == WM_NCLBUTTONDOWN && wp == HTMAXBUTTON)
        SetPropA(h, PROP_CAPMAX, (HANDLE)(ULONG_PTR)GetTickCount());

    /* 打开/关闭窗口菜单 (标题栏图标 / 任务栏右键 / Alt+空格) 必然是用户操作;
       PC 应用自己不会走这条。记录它, 让菜单里的"还原/最大化"能放行。
       关闭时再刷新一次时间戳 —— 菜单命令是在菜单循环退出之后才发过来的,
       用"刚关闭"的 1.5s 窗口判断最准; 菜单一直开着时应用发来的消息不会被误放行。 */
    if (msg == WM_ENTERMENULOOP || msg == WM_INITMENU ||
        msg == WM_EXITMENULOOP)
        SetPropA(h, PROP_MENUTICK, (HANDLE)(ULONG_PTR)GetTickCount());

    if (msg == WM_NCDESTROY) {
        KillTimer(h, TIMER_CAPTION);
        RemovePropA(h, PROP_CAPTIMER);
        ForgetWindowGeom(h);
    }

    /* ★ 用户命令必须"在交给原过程之前"记下来: SC_RESTORE/SC_MAXIMIZE 会在
       DefWindowProc 里同步改窗口几何, 等原过程返回再记就晚了。 */
    if (!g_disabled) {
        if (msg == WM_SYSCOMMAND) {
            UINT c = (UINT)(wp & 0xFFF0);
            BOOL iconic = IsIconic(h) ? TRUE : FALSE;

            if (IsBroadcastWindow(h) &&
                InterlockedIncrement(&g_sysCmdLogs) <= 40)
                HookLog("syscmd hwnd=%p cmd=0x%X iconic=%d taskbar=%d user=%d",
                        (void*)h, c, (int)iconic, (int)TaskbarMinimized(h),
                        (int)UserRestoreSignal(h, 1500));

            /* 锁还在但窗口已经可见/非最小化: 上一次任务栏恢复留下的旧锁
               (系统的恢复不一定发 WM_WINDOWPOSCHANGED)。先解掉, 免得把用户
               接下来的最大化/最小化当成应用请求拦掉。 */
            if (TaskbarMinimized(h) && !iconic && IsWindowVisible(h))
                ReleaseTaskbarMin(h, "window already restored");

            /* ★ 任务栏最小化期间的 SC_RESTORE / SC_MAXIMIZE: 应用(SDL)会周期性
               发这两个命令 —— DefWindowProc 处理它们时**不经过** user32!ShowWindow
               导出, 直接在 user32/win32u 内部把窗口从最小化里拉出来, 我们的
               内联钩子看不到 (16:09 日志: blocked SC_RESTORE 之后紧跟一条
               SC_MAXIMIZE, 窗口就回来了)。
               只有带用户输入特征的恢复才放行, 其余直接吃掉。 */
            if ((c == SC_RESTORE || c == SC_MAXIMIZE) && TaskbarMinimized(h)) {
                if (!UserRestoreSignal(h, 1500)) {
                    if (InterlockedIncrement(&g_minLogs) <= 20)
                        HookLog("blocked %s hwnd=%p (taskbar minimized)",
                                c == SC_MAXIMIZE ? "SC_MAXIMIZE" : "SC_RESTORE",
                                (void*)h);
                    return 0;
                }

                /* ★ 用户真的在恢复: 由我们自己摆回"用户矩形/模拟最大化"。
                   不能只把命令交给 DefWindowProc —— SC_RESTORE 内部的
                   SetWindowPos 会被"几何归用户"的冻结拦掉, 窗口虽然
                   de-iconify, 矩形还停在 (-32000,-32000), 用户看到的就是
                   "点了任务栏图标但窗口没出来"。 */
                RestoreManagedWindow(h, c == SC_MAXIMIZE ? "SC_MAXIMIZE" : "SC_RESTORE");

                /* 从任务栏右键菜单选"最大化": 恢复后直接切到模拟最大化。 */
                if (c == SC_MAXIMIZE && !GetPropA(h, PROP_USERMAX))
                    ToggleEmulatedMax(h);

                return 0;
            }

            /* 已经任务栏最小化时, 应用再发 SC_MINIMIZE 就纯属重复: 直接吃掉,
               否则每来一次都会重新播一遍最小化动画。
               (窗口已经不在最小化状态 -> 锁是旧的, 让正常流程重新最小化。) */
            if (c == SC_MINIMIZE && TaskbarMinimized(h) && IsIconic(h)) {
                if (InterlockedIncrement(&g_minLogs) <= 20)
                    HookLog("ignore app SC_MINIMIZE hwnd=%p (already taskbar-minimized)",
                            (void*)h);
                return 0;
            }

            if (c == SC_RESTORE || c == SC_MINIMIZE ||
                c == SC_MOVE    || c == SC_SIZE) {
                /* 应用发来的 SC_RESTORE (窗口还在最小化) 不能算"用户命令":
                   否则后面的几何保护会把它当成用户还原而放行。
                   SC_MAXIMIZE 不在这里记 —— 它要等确认"用户点的按钮/菜单"
                   之后才由 ToggleEmulatedMax 自己记 (见下)。 */
                if (!(c == SC_RESTORE && iconic))
                    SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());
            }

            /* ★ 最小化 = 把广播窗"收起来"(隐藏), 不是真的 minimize:
               渲染器会不断 ShowWindow(SW_SHOW) 把最小化顶回来, 所以隐藏后
               在 PARKED 期间拦掉应用的显示请求; 恢复方式: 关掉
               「持续保持小窗」(写 bypass_hook.off) 后渲染器会自己重新显示。 */
            if (c == SC_MINIMIZE && (LooksManaged(h) || GetPropA(h, PROP_USERMAX))) {
                if (MinimizeToTaskbarMode()) {
                    /* 真最小化到任务栏。ShowWindow 已被内联 hook, 渲染器
                       再调 SW_SHOW 会被 ShowStateAllowed 拦掉; 样式/SetWindowPos
                       里的"显示"位也被剥掉。
                       ★ 另外打上 TASKBARMIN 锁: 兜住不走这些钩子的恢复路径
                         (DefWindowProc 内部 ShowWindow / 第三方进程调用)。 */
                    SetPropA(h, PROP_TASKBARMIN,
                             (HANDLE)(ULONG_PTR)(GetTickCount() | 1));
                    RemovePropA(h, PROP_CLICKACTIVE);
                    /* 这条 PROP_USERCMD 只是为了"最小化这一下"不被几何冻结挡住;
                       最小化完成后必须清掉 —— 否则接下来 1.5 秒里应用发来的
                       SC_RESTORE/SetWindowPos 会被误当成"用户恢复"而放行。 */
                    SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());
                    InterlockedExchange(&g_inMinimize, 1);
                    if (real_ShowWindow) real_ShowWindow(h, SW_MINIMIZE);
                    InterlockedExchange(&g_inMinimize, 0);
                    RemovePropA(h, PROP_USERCMD);
                    if (InterlockedIncrement(&g_minLogs) <= 10)
                        HookLog("minimize to taskbar hwnd=%p", (void*)h);
                } else {
                    /* 收起: 隐藏, 不占任务栏 (关掉持续保持小窗后渲染器会重新显示) */
                    SetPropA(h, PROP_PARKED, (HANDLE)1);
                    if (real_ShowWindow) real_ShowWindow(h, SW_HIDE);
                    if (InterlockedIncrement(&g_minLogs) <= 10)
                        HookLog("parked hwnd=%p (hidden; toggle sticky off to restore)", (void*)h);
                }
                return 0;
            }

            /* 还原自己做, 不依赖系统的最大化状态:
               渲染器会把 WS_MAXIMIZE 位反复清掉, 那时系统认为"本来就没最大化",
               点还原等于什么都没干, 窗口就永远卡在满屏。
               这里直接摆回"用户矩形"。 */
            if (c == SC_RESTORE && LooksManaged(h) && !IsIconic(h)) {
                RECT ur, cr;
                int  mw = GetSystemMetrics(SM_CXSCREEN);
                int  mh = GetSystemMetrics(SM_CYSCREEN);

                /* ★ 从任务栏恢复"模拟最大化"的窗口: 保持最大化, 不要退成小窗。
                   用户点最小化是允许的; 恢复时如果还带着 BypassUserMax,
                   就重新铺满显示器, 而不是走下面的"用户矩形"还原。 */
                if (GetPropA(h, PROP_USERMAX)) {
                    RECT mr;
                    real_ShowWindow(h, SW_RESTORE);
                    MaxRectFor(h, &mr);
                    real_SetWindowPos(h, NULL, mr.left, mr.top,
                                      mr.right - mr.left, mr.bottom - mr.top,
                                      SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
                    HookLog("user restore hwnd=%p -> kept maximized (from taskbar)",
                            (void*)h);
                    return 0;
                }

                if (GetWindowRect(h, &cr) && GetUserRect(h, &ur) &&
                    (cr.right - cr.left) >= mw * 9 / 10 &&
                    (cr.bottom - cr.top) >= mh * 9 / 10) {
                    real_ShowWindow(h, SW_RESTORE);
                    RemovePropA(h, PROP_USERMAX);
                    CaptionApply(h, TRUE);   /* 控制栏必须回来, 否则小窗拖不动 */
                    real_SetWindowPos(h, NULL, ur.left, ur.top,
                                      ur.right - ur.left, ur.bottom - ur.top,
                                      SWP_NOZORDER | SWP_NOACTIVATE);
                    HookLog("user restore hwnd=%p -> %dx%d@(%d,%d) (done by us)",
                            (void*)h, (int)(ur.right - ur.left),
                            (int)(ur.bottom - ur.top), (int)ur.left, (int)ur.top);
                    return 0;
                }
            }

            /* 最大化用"模拟"实现: 不设 WS_MAXIMIZE, 用普通窗口铺满整个显示器。
               ★ 已经最大化时再点这个按钮**什么都不做**: 按钮画的就是"最大化",
                 不是"还原"。以前这里做 toggle, 用户以为没生效又点一下, 窗口
                 就在大小窗之间来回跳。还原请双击标题栏 (或系统菜单"还原")。 */
            if (c == SC_MAXIMIZE && LooksManaged(h) && !IsIconic(h)) {
                /* 最大化按钮就是开关: 未最大化 -> 铺满; 已最大化 -> 还原小窗。
                   (按钮图标不会变成"还原", 但点它一定二选一, 不会"没反应") */
                /* ★ 任务栏锁还在但窗口已经可见: 这是应用在同步它自己的窗口
                   状态 (被我们拦掉 SC_RESTORE 之后它会补一条 SC_MAXIMIZE),
                   不是用户点了最大化按钮。用户真正点按钮之前, 任务栏那次
                   点击已经通过 WA_CLICKACTIVE 把锁解掉了。
                   不区分的话, 应用会把用户刚恢复的小窗又 toggle 成满屏 / 或把
                   满屏 toggle 回小窗。 */
                if (TaskbarMinimized(h)) {
                    if (InterlockedIncrement(&g_minLogs) <= 20)
                        HookLog("ignore app SC_MAXIMIZE hwnd=%p (taskbar lock stale)",
                                (void*)h);
                    return 0;
                }
                /* 应用自己每 100~800ms 也会发 SC_MAXIMIZE (SDL 在同步窗口状态),
                   如果无条件 toggle, 用户刚恢复的小窗会自己跳成满屏。
                   只有"刚点过标题栏最大化按钮"或"刚从这个窗口的菜单里选了
                   最大化"才算用户操作。 */
                {
                    DWORD cap = (DWORD)(ULONG_PTR)GetPropA(h, PROP_CAPMAX);
                    DWORD mnu = (DWORD)(ULONG_PTR)GetPropA(h, PROP_MENUTICK);
                    DWORD now = GetTickCount();
                    BOOL  byUser = (cap && (DWORD)(now - cap) <= 3000) ||
                                   (mnu && (DWORD)(now - mnu) <= 2000);

                    RemovePropA(h, PROP_CAPMAX);
                    if (!byUser) {
                        if (InterlockedIncrement(&g_minLogs) <= 20)
                            HookLog("ignore app SC_MAXIMIZE hwnd=%p (no user click/menu)",
                                    (void*)h);
                        return 0;
                    }
                }
                ToggleEmulatedMax(h);
                return 0;
            }
        } else if (msg == WM_NCLBUTTONDBLCLK) {
            /* 双击标题栏 = 最大化/还原 (同样用模拟实现)。
               ★ 刚从任务栏/缩略图恢复的 1.2 秒内忽略: 缩略图的点击会被转发成
                 双击, 不忽略的话"从最大化最小化再点回来"会被 toggle 成小窗。 */
            {
                DWORD rt = (DWORD)(ULONG_PTR)GetPropA(h, PROP_RESTORED);
                if (rt && (DWORD)(GetTickCount() - rt) < 1200)
                    return 0;
            }
            SetPropA(h, PROP_USERCMD, (HANDLE)(ULONG_PTR)GetTickCount());
            if (LooksManaged(h) && !IsIconic(h)) {
                ToggleEmulatedMax(h);
                return 0;
            }
        }
    }

    /* 先让原窗口过程处理, 我们再改它的结论 —— 顺序不能反:
       原过程会把 ptMinTrackSize 设成全屏, 我们在它之后清零才有效。 */
    if (old) r = CallWindowProcW(old, h, msg, wp, lp);
    else     r = DefWindowProcW(h, msg, wp, lp);

    if (msg == WM_ENTERSIZEMOVE) {
        SetPropA(h, PROP_SIZING, (HANDLE)1);
        return r;
    }    if (msg == WM_EXITSIZEMOVE) {
        RECT ur;
        RemovePropA(h, PROP_SIZING);
        if (GetWindowRect(h, &ur)) {
            if (IsWorkAreaSized(h, &ur) || IsMonitorSized(h, &ur)) {
                /* 拖动的仍是"铺满屏幕"的窗口: 保留 BypassUserMax 和原来的
                   小窗还原点 —— 否则一拖就会被 enforcer 降回小窗。 */
                TrackGeo(h, &ur);
            } else {
                RemovePropA(h, PROP_USERMAX);
                CaptionApply(h, TRUE);   /* 用户把最大化窗口拖小了: 控制栏要回来 */
                TrackGeo(h, &ur);
                SetUserRect(h, &ur);  /* 用户摆好的矩形 = 以后"还原"的目标 */
                HookLog("user-resized hwnd=%p -> %dx%d@(%d,%d) managed=%d",
                        (void*)h, (int)(ur.right - ur.left), (int)(ur.bottom - ur.top),
                        (int)ur.left, (int)ur.top, (int)IsManaged(h));
            }
        }
        return r;
    }

    if (msg == WM_BYPASS_FIX) {
        /* 用户主动最大化的窗口: 任何 enforcer 消息一律忽略 */
        if (GetPropA(h, PROP_USERMAX)) return 0;
        FixOnOwnerThread(h, (int)wp);
        return 0;
    }

    if (g_disabled || !lp) return r;

    if (msg == WM_GETMINMAXINFO) {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        if (GetAncestor(h, GA_ROOT) == h && IsBroadcastWindow(h)) {
            /* SDL 对"非 resizable"窗口会把 min 和 max 都钉成当前尺寸,
               于是拖不大、也拉不小、最大化按钮完全没反应。这里两头都放开。 */
            mmi->ptMinTrackSize.x = 160;
            mmi->ptMinTrackSize.y = 120;
            mmi->ptMaxTrackSize.x = GetSystemMetrics(SM_CXSCREEN);
            mmi->ptMaxTrackSize.y = GetSystemMetrics(SM_CYSCREEN);
        }
        return r;
    }

    /* 光标: SDL 在客户区里会 SetCursor(NULL) 把光标抹掉 (应用没设自定义光标)。
       别的进程的 IAT 我们不一定都打到, 所以在窗口过程这层再兜一次。 */
    if (msg == WM_SETCURSOR) {
        if (LOWORD((DWORD_PTR)lp) == HTCLIENT && r == TRUE && !GetCursor()) {
            HCURSOR arrow = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
            if (arrow) SetCursor(arrow);
            return TRUE;
        }
        return r;
    }

    if (msg == WM_NCCALCSIZE) {
        /* SDL 用 SDL_WINDOW_BORDERLESS 建窗口时会处理 WM_NCCALCSIZE,
           把客户区撑满整个窗口 —— 结果标题栏/边框全看不见, 拖边也改不了大小。
           但这里不能无差别地整段绕过 SDL: 只对"已经补上标准外框"的窗口
           走系统默认计算, 还没外框的窗口保留 SDL 自己的处理 (IME/输入区域
           相关代码还在里面)。 */
        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);
        if (wp && (st & (WS_CAPTION | WS_THICKFRAME)))
            return DefWindowProcW(h, msg, wp, lp);
        return r;
    }

    if (msg == WM_STYLECHANGED) {
        /* 渲染器自己会把 CAPTION/THICKFRAME 去掉, 补回来。
           重入标记用**窗口属性**而不是进程级静态变量 —— 静态变量是
           全进程共享的, 两个窗口同时收到 STYLECHANGED 时其中一个会被
           静默吞掉。 */
        static const char* STY = "BypassSty";
        LONG_PTR st = GetWindowLongPtrW(h, GWL_STYLE);

        if (IsBroadcastWindow(h) && !(st & WS_CAPTION) &&
            !GetPropA(h, STY) && !GetPropA(h, PROP_CAPHIDE) && !IsIconic(h)) {
            SetPropA(h, STY, (HANDLE)1);
            st |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
            /* ★ 只摘 WS_POPUP, 绝不能顺手摘 WS_MAXIMIZE:
               用户点了最大化 -> 渲染器下一轮又把 "POPUP" 样式写回来 ->
               我们在这里补外框时若把 MAXIMIZE 一起清掉, 窗口就会从最大化
               掉回小窗 (日志实证: st 0x15CF0000 -> 0x14CF0000)。
               清 MAXIMIZE 只在"第一次降级全屏窗口"时做, 见 DemoteWindowBase。 */
            st &= ~(LONG_PTR)WS_POPUP;
            real_SetWindowLongW(h, GWL_STYLE, (LONG)st);
            real_SetWindowPos(h, NULL, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                              SWP_NOACTIVATE | SWP_FRAMECHANGED);
            HookLog("re-asserted style hwnd=%p st=0x%08X", (void*)h, (unsigned)st);
            RemovePropA(h, STY);
        }
        return r;
    }

    if (msg == WM_WINDOWPOSCHANGING) {
        WINDOWPOS* wpos = (WINDOWPOS*)lp;

        if (GetAncestor(h, GA_ROOT) != h) return r;

        /* ★ 不是广播窗口 (学生端自己的工具条/虚拟桌面面板/对话框) 一律原样放过 */
        if (!IsBroadcastWindow(h)) return r;

        /* 已"收起"(隐藏): 应用的任何 SetWindowPos 都不能把它重新显示/挪动 */
        if (GetPropA(h, PROP_PARKED)) {
            wpos->flags |= SWP_NOSIZE | SWP_NOMOVE;
            wpos->flags &= ~(UINT)(SWP_SHOWWINDOW | SWP_STATECHANGED);
        }
        /* 最小化期间: 应用的 SetWindowPos 也不许把它还原。
           ★ 但"用户从任务栏恢复"必须整段放行: 只剥 SWP_STATECHANGED 会让
             窗口画出来、内部却仍是 IsIconic 的僵尸状态 —— 表现就是
             "窗口弹回来了，但最大化/还原按钮全都没反应，小窗像死了"。
             判断用户: 刚有用户命令 (SC_RESTORE / 点任务栏 / WA_CLICKACTIVE)
             或光标在任务栏/缩略图上且有输入。 */
        if (LooksManaged(h) && IsIconic(h)) {
            DWORD lockTick = TaskbarMinimized(h)
                             ? (DWORD)(ULONG_PTR)GetPropA(h, PROP_TASKBARMIN)
                             : 0;
            DWORD nowTick  = GetTickCount();
            BOOL  inMin    = (InterlockedCompareExchange(&g_inMinimize, 0, 0) != 0);
            /* ★ 锁刚打上 (<800ms) = 这就是"用户刚点最小化"引发的那一次
               状态变更 (SC_MINIMIZE 会先写 PROP_USERCMD, 所以 UserCmdRecent
               在这时一定是 true)。这时既不能剥标志位, 也不能解锁,
               否则任务栏锁会在最小化的一瞬间被自己解掉。 */
            BOOL freshLock = (lockTick &&
                              (DWORD)(nowTick - lockTick) < 800) ? TRUE : FALSE;
            BOOL userRestore = (!inMin && !freshLock &&
                                (UserCmdRecent(h, 1500) ||
                                 UserRestoreSignal(h, 1500))) ? TRUE : FALSE;

            if (inMin || freshLock || userRestore) {
                /* 最小化进行中 / 用户恢复: 不剥任何标志。
                   ⚠ 这里**不解锁** —— 解锁放到 WM_WINDOWPOSCHANGED
                   (确认窗口真的已经不是 iconic 了) 或 WA_CLICKACTIVE/SC_RESTORE
                   里做, 避免在最小化过程中把锁误删。 */
            } else {
                wpos->flags |= SWP_NOSIZE | SWP_NOMOVE;
                wpos->flags &= ~(UINT)(SWP_SHOWWINDOW | SWP_HIDEWINDOW |
                                       SWP_STATECHANGED);
            }
        }

        /* ★ 拖动/缩放"模拟最大化"的窗口时: 只拦应用那条"精确等于整屏"的
           全屏请求 (它会瞬间把 1040x719 改成 1024x703, 露出缝隙)。
           用户自己拖边框改大小不受影响 (尺寸是连续变化, 不会正好等于整屏)。 */
        if (GetPropA(h, PROP_USERMAX) && InSizeMove(h) &&
            !(wpos->flags & SWP_NOSIZE) &&
            wpos->cx == GetSystemMetrics(SM_CXSCREEN) &&
            wpos->cy == GetSystemMetrics(SM_CYSCREEN)) {
            wpos->flags |= SWP_NOSIZE | SWP_NOMOVE;
        }

        /* 置顶请求一律改成不置顶 (几何交给下面的统一规则) */
        if (wpos->hwndInsertAfter == HWND_TOPMOST)
            wpos->hwndInsertAfter = HWND_NOTOPMOST;

        /* ★ 接管过的窗口: 几何归用户。
           只放行三种"用户级"的变化, 其余一律冻住:
             a) 用户正在拖边框/拖标题栏 (WM_ENTERSIZEMOVE..EXITSIZEMOVE);
             b) 系统正在最大化它 (请求尺寸 >= 整屏, 且样式里还没 WS_MAXIMIZE);
             c) 系统正在把它从最大化还原 (样式里的 WS_MAXIMIZE 刚被清掉,
                或请求的矩形正好等于我们记录的"用户当前矩形")。
           其它情况 (渲染器把缓存的 852x479 再发一遍 / 500ms 定时器挪回
           sw/8 / 每轮重写样式带出来的尺寸变化) 全部挡掉。 */
        if (LooksManaged(h) && !InSizeMove(h)) {
            int  mw = GetSystemMetrics(SM_CXSCREEN);
            int  mh = GetSystemMetrics(SM_CYSCREEN);
            BOOL userCmd    = UserCmdRecent(h, 1500);
            /* ★ 正在执行"最小化到任务栏"的 ShowWindow: 系统会把窗口挪到
               (-32000,-32000) 160x28 —— 这是标准的"已最小化"位置, 必须放行。
               以前 USERMAX 会让下面的冻结无条件生效, 结果窗口被留在满屏
               矩形上: 看起来最小化了, 但窗口仍然盖着整个屏幕, 点一下顶边
               就会命中它 → 又"弹回来"。 */
            BOOL minimizing = (InterlockedCompareExchange(&g_inMinimize, 0, 0) != 0);
            /* 系统把窗口挪到 (-32000,-32000) 的标准"已最小化"位置: 任何情况下
               都要放行, 否则窗口会留在满屏矩形上, 点击屏幕任何地方都可能命中
               这个"看不见但还在"的窗口, 又把它带回来。 */
            BOOL toIconic = (!(wpos->flags & SWP_NOMOVE) &&
                             wpos->x <= -30000 && wpos->y <= -30000) ? TRUE : FALSE;
            /* 最大化/还原都必须"用户发起": 渲染器自己每 500ms 想在满屏和它
               缓存的小矩形之间来回切, 那不是用户意愿。
               注意 restoring 只看"用户命令", 不看 WS_MAXIMIZE ——
               渲染器会反复改写样式把最大化位清掉, 那个位不可靠;
               几何是否放行只由"用户刚动过"决定。 */
            BOOL maximizing = (wpos->cx >= mw && wpos->cy >= mh) && userCmd;
            BOOL restoring  = userCmd && !maximizing;

            /* ★ 已经在"模拟最大化"状态: 应用发来的任何几何变更一律冻住。
               渲染器时不时会把窗口摆成 "-8,-8 1920x1080" —— 外框贴住屏幕左上角,
               右下却空出 8px, 就是用户看到的"缝隙"。我们自己的满屏几何是
               "-8,-8 1936x1096"(边框推到屏幕外), 不能让应用把它改回去。
               还原时 USERMAX 已先被摘掉 (见 ToggleEmulatedMax), 不受影响。
               ⚠ 最小化过渡 (minimizing) 例外: 那是系统在摆放最小化位置。 */
            if (!toIconic &&
                ((GetPropA(h, PROP_USERMAX) && !minimizing) ||
                 (!maximizing && !restoring))) {
                if (!(wpos->flags & SWP_NOSIZE) || !(wpos->flags & SWP_NOMOVE)) {
                    if (InterlockedIncrement(&g_clampLogs) <= 24)
                        HookLog("keep user geometry hwnd=%p (app wanted %d,%d %dx%d flags=0x%X)",
                                (void*)h, wpos->x, wpos->y, wpos->cx, wpos->cy,
                                wpos->flags);
                }
                wpos->flags |= SWP_NOSIZE | SWP_NOMOVE;
                /* ★ 连 SWP_FRAMECHANGED 一起吞掉: 应用每轮都带这个标志重算边框,
                   窗口管理器会因此把 WS_MAXIMIZE 状态丢掉 —— 于是窗口变成
                   "普通窗口 + 满屏几何", 标题栏被顶到 (-8,-8) 屏幕外,
                   最大化/还原按钮看不见也点不到, 表现就是"窗口卡死"。 */
                wpos->flags &= ~(UINT)SWP_FRAMECHANGED;
            }
        }

        /* 渲染器的 500ms 定时器会把窗口挪回 (sw/8, sh/8) —— 只有位置、没尺寸。
           用户已经摆好位置之后, 这一下就是"位置被拉回"。这里专门挡掉它:
           请求里带 SWP_NOSIZE 且落点正好是 sw/8,sh/8 就不移动。 */
        if (LooksManaged(h) && !InSizeMove(h) && (wpos->flags & SWP_NOSIZE) &&
            !(wpos->flags & SWP_NOMOVE)) {
            int px = GetSystemMetrics(SM_CXSCREEN) / 8;
            int py = GetSystemMetrics(SM_CYSCREEN) / 8;
            int dx = wpos->x - px, dy = wpos->y - py;

            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            if (dx <= 2 && dy <= 2) {
                wpos->flags |= SWP_NOMOVE;
                if (InterlockedIncrement(&g_clampLogs) <= 20)
                    HookLog("kept user position hwnd=%p (app wanted %d,%d)",
                            (void*)h, px, py);
            }
        }

        /* 只拦"直接跳到满屏"的那一次 (渲染器创建时的形状)。
           一旦我们接管过这个窗口, 之后用户拖大/最大化都不再插手。 */
        if (!LooksManaged(h) && !(wpos->flags & SWP_NOSIZE) &&
            (wpos->cx >= GetSystemMetrics(SM_CXSCREEN) ||
             wpos->cy >= GetSystemMetrics(SM_CYSCREEN))) {
            int rqW = wpos->cx, rqH = wpos->cy;
            int nx, ny, nw, nh;
            SmallRectFor(rqW, rqH, &nx, &ny, &nw, &nh);
            wpos->x = nx;
            wpos->y = ny;
            wpos->cx = nw;
            wpos->cy = nh;
            if (InterlockedIncrement(&g_clampLogs) <= 20) {
                char cls[64] = "?";
                GetClassNameA(h, cls, sizeof(cls));
                HookLog("wpchg clamp hwnd=%p cls=%s req=%dx%d -> %dx%d",
                        (void*)h, cls, rqW, rqH, nw, nh);
            }
        }
        return r;
    }

    if (msg == WM_WINDOWPOSCHANGED) {
        /* 事件驱动: 每次几何变化都记一行 (1 秒采样抓不到"拖大→立刻改回")。 */
        WINDOWPOS* wpos = (WINDOWPOS*)lp;
        RECT       nr;

        if (GetAncestor(h, GA_ROOT) == h && GetWindowRect(h, &nr) &&
            InterlockedIncrement(&g_geoLogs2) <= 40) {
            HookLog("wpchanged hwnd=%p now=%dx%d@(%d,%d) flags=0x%X managed=%d st=0x%08X",
                    (void*)h, (int)(nr.right - nr.left), (int)(nr.bottom - nr.top),
                    (int)nr.left, (int)nr.top,
                    wpos ? wpos->flags : 0, (int)IsManaged(h),
                    (unsigned)GetWindowLongPtrW(h, GWL_STYLE));
        }

        /* ★ 任务栏最小化锁: 应用/DefWindowProc 仍可能从我们钩不到的路径把
           窗口还原 (实测: 最小化后 0.5~1s 自己弹回来)。在
           WM_WINDOWPOSCHANGED 里兜底: 没有用户输入特征就立刻再压回去。
           这一步发生在 SetWindowPos 内部、窗口重画之前, 正常看不到闪烁。 */
        /* 只在"确实是从最小化里被拉回来"时兜底 (PROP_ICONIC 是上一次
           最小化时记下的)。否则最小化那一下会把自己刚上的锁解掉。 */
        if (!g_disabled && GetAncestor(h, GA_ROOT) == h && TaskbarMinimized(h) &&
            GetPropA(h, PROP_ICONIC) && !IsIconic(h)) {
            if (UserRestoreSignal(h, 1500)) {
                ReleaseTaskbarMin(h, "window pos changed");
                /* 窗口可能只是 de-iconify 了、矩形还在 -32000:
                   投递一次"用户恢复", 确保它真的摆回屏幕内。 */
                PostMessageA(h, WM_BYPASS_UNPARK, 0, 0);
            } else if (InterlockedCompareExchange(&g_reminimizing, 1, 0) == 0) {
                if (InterlockedIncrement(&g_minLogs) <= 20)
                    HookLog("re-minimize hwnd=%p (restore bypassed the hooks)",
                            (void*)h);
                if (real_ShowWindow) real_ShowWindow(h, SW_MINIMIZE);
                InterlockedExchange(&g_reminimizing, 0);
            }
        }

        /* 记住"这一刻是不是最大化状态": 下一次 WM_WINDOWPOSCHANGING 里
           用它区分"系统在还原" 和 "渲染器想把窗口按回小尺寸"。
           ★ 注意: 最大化现在走"模拟"实现 (不设 WS_MAXIMIZE), 所以这里
             绝不能再把 WS_MAXIMIZE 补回样式 —— 否则又会回到
             "样式是最大化、几何是小窗"的错位状态, 标题栏按钮会全部失效。 */
        if (GetAncestor(h, GA_ROOT) == h) {
            BOOL zoomNow = IsZoomed(h) ? TRUE : FALSE;
            BOOL zoomWas = GetPropA(h, PROP_ZOOMED) != NULL;
            BOOL iconNow = IsIconic(h) ? TRUE : FALSE;

            /* 最小化/恢复: 区分用户点任务栏 vs 应用自己恢复 (见 HandleIconicTransition) */
            HandleIconicTransition(h, iconNow);

            if (zoomNow != zoomWas) {
                /* ★ 一次用户命令只放行一次"最大化/还原"切换。
                   少了这句, 用户点最大化后渲染器在 0.5 秒内跟进的还原
                   仍然落在 1.5 秒宽限里, 于是又被它退掉 (日志实证:
                   keep user geometry -> 0.5s 后 441x466 st=0x14CF0000)。 */
                RemovePropA(h, PROP_USERCMD);
            }
            if (zoomNow) SetPropA(h, PROP_ZOOMED, (HANDLE)1);
            else         RemovePropA(h, PROP_ZOOMED);
        }
        return r;
    }

    return r;
}

/* ---- virtual desktop pinning (see bypass_vd.inc) ---- */
#include "bypass_vd.inc"

/* ---- window-mode (control-side) small broadcast window ----
   The teacher's console can send the broadcast in window mode: the renderer
   then creates the render window ALREADY SMALL (WS_EX_TOPMOST + caption).
   Nothing demotes it, so the topmost flag it was born with would stay
   forever, and every app re-assert of HWND_TOPMOST (which we turn into
   HWND_NOTOPMOST) lifted it above the student's active window again.
   1 = freeze app-driven z-order for this window (user clicks still raise
   it normally, because then it is the foreground window). */
static BOOL SmallBcastNoRaise(HWND h)
{
    char cls[64];

    if (!h || !IsWindow(h)) return FALSE;
    if (GetForegroundWindow() == h) return FALSE;   /* active: raising is harmless */
    if (LooksManaged(h)) return FALSE;              /* demoted window: own rules */
    if (IsWorthFixing(h)) return FALSE;             /* fullscreen-like: demote path */
    cls[0] = 0;
    GetClassNameA(h, cls, sizeof(cls));
    if (!IsRenderWindowClass(cls)) return FALSE;    /* class-based only */
    if (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST) return FALSE;
    return TRUE;                                    /* plain small broadcast window */
}

static BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after,
                                     int x, int y, int cx, int cy, UINT flags)
{
    BOOL changed = FALSE;

    /* virtual desktop: pin before SWP_SHOWWINDOW can make it visible, and
       before VD_NoActivate() is consulted (a fresh process still has to
       resolve the target, and the first show is the critical one).
       Not gated on IsBroadcastWindow(): the window-mode small window must be
       covered as well. */
    if (!g_disabled && (flags & SWP_SHOWWINDOW) != 0)
        VD_PinNow(h, "setwindowpos");

    /* virtual desktop: never let a call activate a render window sitting on a
       desktop the user is not looking at - activation is what makes the shell
       jump to that desktop. */
    if (!g_disabled && (flags & SWP_NOACTIVATE) == 0 && VD_NoActivate(h)) {
        flags |= SWP_NOACTIVATE;
        if (VD_LogWanted(h, VD_LOG_SWP, VD_LOG_MS))
            HookLog("vd: forced SWP_NOACTIVATE hwnd=%p (pinned desktop is not current)",
                    (void*)h);
    }

    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h)) {
        if (GetPropA(h, PROP_PARKED))
            flags &= ~(UINT)SWP_SHOWWINDOW;
        if (TaskbarMinimized(h))
            flags &= ~(UINT)(SWP_SHOWWINDOW | SWP_STATECHANGED);
        if (LooksManaged(h)) {
            flags &= ~(UINT)SWP_HIDEWINDOW;         /* 广播窗不许被应用隐藏 */
            if (IsIconic(h))
                flags &= ~(UINT)SWP_SHOWWINDOW;     /* 最小化时不许用 SetWindowPos 还原 */
        }

        /* 取证: 每次真正改尺寸/位置的调用都记下来, 并标出调用者模块。
           ⚠ 条件必须是"两个都设了才跳过": 渲染器的 500ms 定时器调的是
           SetWindowPos(..., SWP_NOSIZE) —— 只改位置不改尺寸, 以前被这条
           条件漏掉了, 而这正是"位置被拉回 (sw/8, sh/8)"的来源。 */
        if ((flags & (SWP_NOSIZE | SWP_NOMOVE)) != (SWP_NOSIZE | SWP_NOMOVE) &&
            InterlockedIncrement(&g_swpLogs) <= 60) {
            const char* mod = "?";
            char  name[MAX_PATH] = "?";
            HMODULE m = NULL;
            void* ra = __builtin_return_address(0);

            if (ra && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         (LPCSTR)ra, &m) && m) {
                GetModuleFileNameA(m, name, sizeof(name));
                { char* p = strrchr(name, '\\'); mod = p ? p + 1 : name; }
            }
            HookLog("swp hwnd=%p req=(%d,%d) %dx%d flags=0x%X after=%p caller=%s+0x%lX",
                    (void*)h, x, y, cx, cy, flags, (void*)after,
                    mod, m ? (unsigned long)((char*)ra - (char*)m) : 0UL);
        }
        /* 1) 绝不允许置顶 (HWND_NOTOPMOST 才是唯一能清 WS_EX_TOPMOST 的)。
           注意不要动 HWND_TOP: 那是"置顶到 Z 序最前", 不改变 topmost 属性,
           改掉它会把应用合法的 Z 序请求弄坏。 */
        /* virtual desktop: pinned at the top of this function (before the
           activation check), nothing left to do here */
        if (after == HWND_TOPMOST) {
            if (SmallBcastNoRaise(h)) {
                /* plain small broadcast window: freeze z-order, do not raise */
                after = NULL;
                flags |= SWP_NOZORDER;
            } else
            after = HWND_NOTOPMOST;
            changed = TRUE;
        }

        /* ★ 接管过的窗口: 几何完全归用户。
           实测 (13:29 日志): 用户点最大化后, 渲染器立刻
             SetWindowPos(hwnd, HWND_TOPMOST, 1682,743, 852,479, SWP_FRAMECHANGED)
           把窗口按回它缓存的那份小尺寸 —— 这就是"最大化又被拉回去"的原因。
           它所有的尺寸/位置请求一律改成"保持现状" (只保留 Z 序/显示/激活语义)。
           用户正在拖边框时放行 —— 那时应用可能在自己的 WM_SIZING 里调
           SetWindowPos 做吸附, 拦掉会让拖动手感变怪。 */
        if (LooksManaged(h) && IsTargetWindow(h) && !InSizeMove(h)) {
            if (!(flags & SWP_NOSIZE) || !(flags & SWP_NOMOVE)) {
                if (InterlockedIncrement(&g_clampLogs) <= 24)
                    HookLog("keep user geometry hwnd=%p (app wanted %d,%d %dx%d flags=0x%X)",
                            (void*)h, x, y, cx, cy, flags);
            }
            flags |= SWP_NOSIZE | SWP_NOMOVE;
            flags &= ~(UINT)SWP_FRAMECHANGED;   /* 同上: 别让它重算边框, 否则丢最大化状态 */
            changed = TRUE;
        }

        /* 2) 只拦"直接跳到满屏"的那一次; 接管过的窗口交给用户 */
        if (!LooksManaged(h) && !(flags & SWP_NOSIZE) &&
            (cx >= GetSystemMetrics(SM_CXSCREEN) ||
             cy >= GetSystemMetrics(SM_CYSCREEN))) {
            int rqW = cx, rqH = cy;
            int nx, ny, nw, nh;
            SmallRectFor(rqW, rqH, &nx, &ny, &nw, &nh);
            x = nx;
            y = ny;
            cx = nw;
            cy = nh;
            flags &= ~(UINT)(SWP_NOSIZE | SWP_NOMOVE);
            changed = TRUE;
            if (InterlockedIncrement(&g_clampLogs) <= 15) {
                DWORD opid = 0;
                char  img[MAX_PATH] = "?", cls[64] = "?";
                GetWindowThreadProcessId(h, &opid);
                GetClassNameA(h, cls, sizeof(cls));
                if (opid != g_pid) GetPidImage(opid, img, sizeof(img));
                else               _snprintf(img, sizeof(img) - 1, "%s", g_ownImage);
                HookLog("clamped req=%dx%d -> %dx%d (hwnd=%p owner=%s cls=%s)",
                        rqW, rqH, cx, cy, (void*)h, img, cls);
            }
        }
        /* 只在真的动过参数时才加 NOACTIVATE, 不要吞掉应用合法的激活请求 */
        if (changed) flags |= SWP_NOACTIVATE;
    }
    return real_SetWindowPos(h, after, x, y, cx, cy, flags);
}

/* 只有"像广播窗"的窗口才套用我们的样式; 否则学生端自己的小工具窗
   (StudentHelperWnd 之类) 也会被加上标题栏。 */
static BOOL ShouldRestyle(HWND h, LONG_PTR val)
{
    RECT r;
    int  sw = GetSystemMetrics(SM_CXSCREEN);
    int  sh = GetSystemMetrics(SM_CYSCREEN);

    if (val & WS_POPUP) return TRUE;
    if (!GetWindowRect(h, &r)) return FALSE;
    return ((r.right - r.left) >= sw * 2 / 3 || (r.bottom - r.top) >= sh * 2 / 3);
}

/* 统一"消毒"渲染器想套上去的样式。
   ★ 接管过的窗口不能清 WS_MAXIMIZE: 用户刚点了最大化, 渲染器又把它缓存的
     旧样式写回来时, 如果我们顺手把 WS_MAXIMIZE 摘掉, 窗口就会从最大化
     "掉"回普通大小 —— 这正是"拖大/最大化后被自动拉回"的一条路径。 */
static LONG_PTR SanitizeStyle(HWND h, LONG_PTR val)
{
    /* ★ 模拟最大化不使用 WS_MAXIMIZE。应用每轮都会把缓存的旧样式写回来,
       有时还带着 WS_MAXIMIZE —— 一旦放行, 窗口就变成"样式是最大化、几何
       是小窗", 标题栏按钮/命中测试全错位 (经典卡死)。这里一律剥掉。 */
    val &= ~(LONG_PTR)(WS_POPUP | WS_MAXIMIZE);
    if (LooksManaged(h)) {
        /* ★ 最小化期间不要改样式: 任何非客户区变化 (哪怕只是补 CAPTION)
           都可能让系统立刻把窗口"还原" —— 这是 "点最小化又被拉回来" 的另一条路径。 */
        if (IsIconic(h))
            return GetWindowLongPtrW(h, GWL_STYLE);
        /* "收起"(隐藏)期间不能让应用通过样式写回 WS_VISIBLE 把窗口显示出来 */
        if (GetPropA(h, PROP_PARKED))
            val &= ~(LONG_PTR)WS_VISIBLE;
        else
            val |= (LONG_PTR)WS_VISIBLE;   /* 不让应用写回"无 WS_VISIBLE"把窗口隐藏 */
        val &= ~(LONG_PTR)WS_MINIMIZE;
    }
    val |= WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    /* ★ 标题栏自动隐藏状态: 应用每轮写样式时不能把 WS_CAPTION 加回来,
       否则控制栏刚藏好又冒出来。正常状态仍然补齐标题栏。 */
    if (!GetPropA(h, PROP_CAPHIDE))
        val |= WS_CAPTION;
    else
        val &= ~(LONG_PTR)WS_CAPTION;
    return val;
}

static LONG WINAPI Hook_SetWindowLongW(HWND h, int idx, LONG val)
{
    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h)) {
        if (idx == GWL_STYLE && (LooksManaged(h) || ShouldRestyle(h, val))) {
            val = (LONG)SanitizeStyle(h, (LONG_PTR)val);
        } else if (idx == GWL_EXSTYLE) {
            val &= ~(LONG)WS_EX_TOPMOST;
        }
    }
    return real_SetWindowLongW(h, idx, val);
}

static LONG WINAPI Hook_SetWindowLongA(HWND h, int idx, LONG val)
{
    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h)) {
        if (idx == GWL_STYLE && (LooksManaged(h) || ShouldRestyle(h, val))) {
            val = (LONG)SanitizeStyle(h, (LONG_PTR)val);
        } else if (idx == GWL_EXSTYLE) {
            val &= ~(LONG)WS_EX_TOPMOST;
        }
    }
    return real_SetWindowLongA(h, idx, val);
}

static LONG_PTR WINAPI Hook_SetWindowLongPtrW(HWND h, int idx, LONG_PTR val)
{
    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h)) {
        if (idx == GWL_STYLE && (LooksManaged(h) || ShouldRestyle(h, val))) {
            val = SanitizeStyle(h, val);
        } else if (idx == GWL_EXSTYLE) {
            val &= ~(LONG_PTR)WS_EX_TOPMOST;
        }
    }
    return real_SetWindowLongPtrW(h, idx, val);
}

static LONG_PTR WINAPI Hook_SetWindowLongPtrA(HWND h, int idx, LONG_PTR val)
{
    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h)) {
        if (idx == GWL_STYLE && (LooksManaged(h) || ShouldRestyle(h, val))) {
            val = SanitizeStyle(h, val);
        } else if (idx == GWL_EXSTYLE) {
            val &= ~(LONG_PTR)WS_EX_TOPMOST;
        }
    }
    return real_SetWindowLongPtrA(h, idx, val);
}

static BOOL WINAPI Hook_MoveWindow(HWND h, int x, int y, int w, int hgt, BOOL repaint)
{
    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h)) {
        if (InterlockedIncrement(&g_mwLogs) <= 40) {
            const char* mod = "?";
            char  name[MAX_PATH] = "?";
            HMODULE m = NULL;
            void* ra = __builtin_return_address(0);

            if (ra && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                         GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                         (LPCSTR)ra, &m) && m) {
                GetModuleFileNameA(m, name, sizeof(name));
                { char* p = strrchr(name, '\\'); mod = p ? p + 1 : name; }
            }
            HookLog("movewin hwnd=%p req=(%d,%d) %dx%d managed=%d caller=%s+0x%lX",
                    (void*)h, x, y, w, hgt, (int)IsManaged(h),
                    mod, m ? (unsigned long)((char*)ra - (char*)m) : 0UL);
        }
    }
    if (!g_disabled && IsTargetWindow(h) && LooksManaged(h) && IsBroadcastWindow(h) &&
        !InSizeMove(h)) {
        /* 接管后几何归用户: MoveWindow 一定会改尺寸, 直接不执行 */
        if (InterlockedIncrement(&g_clampLogs) <= 24)
            HookLog("keep user geometry hwnd=%p (MoveWindow %dx%d ignored)",
                    (void*)h, w, hgt);
        return TRUE;
    }
    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h) && !LooksManaged(h) &&
        (w >= GetSystemMetrics(SM_CXSCREEN) || hgt >= GetSystemMetrics(SM_CYSCREEN))) {
        SmallRectFor(w, hgt, &x, &y, &w, &hgt);
    }
    return real_MoveWindow(h, x, y, w, hgt, repaint);
}

static BOOL WINAPI Hook_SetWindowPlacement(HWND h, const WINDOWPLACEMENT* wp)
{
    WINDOWPLACEMENT tmp;

    /* virtual desktop: SetWindowPlacement can show *and activate* a window
       without going through ShowWindow(), so it needs the same treatment.
       Handled before the broadcast-window logic so the window-mode small
       window is covered too. */
    if (!g_disabled && wp && VD_NoActivate(h) &&
        (wp->showCmd == SW_SHOW || wp->showCmd == SW_SHOWNORMAL ||
         wp->showCmd == SW_SHOWDEFAULT || wp->showCmd == SW_RESTORE ||
         wp->showCmd == SW_SHOWMAXIMIZED || wp->showCmd == SW_MAXIMIZE)) {
        WINDOWPLACEMENT vd = *wp;
        if (VD_LogWanted(h, VD_LOG_PLACEMENT, VD_LOG_MS))
            HookLog("vd: SetWindowPlacement(show=%d) -> SW_SHOWNOACTIVATE hwnd=%p "
                    "(pinned desktop is not current)", wp->showCmd, (void*)h);
        vd.showCmd = SW_SHOWNOACTIVATE;
        return real_SetWindowPlacement(h, &vd);
    }

    if (!g_disabled && IsTargetWindow(h) && wp && IsBroadcastWindow(h)) {
        int rqW, rqH, nx, ny, nw, nh;

        tmp = *wp;
        if (GetPropA(h, PROP_PARKED)) return TRUE;   /* 收起期间不接受任何摆放/显示 */
        if (TaskbarMinimized(h)) return TRUE;        /* 任务栏最小化锁: 同上 */
        rqW = tmp.rcNormalPosition.right - tmp.rcNormalPosition.left;
        rqH = tmp.rcNormalPosition.bottom - tmp.rcNormalPosition.top;

        if (InterlockedIncrement(&g_setwpLogs) <= 40)
            HookLog("setwp hwnd=%p req=%dx%d at (%d,%d) show=%d managed=%d",
                    (void*)h, rqW, rqH, (int)tmp.rcNormalPosition.left,
                    (int)tmp.rcNormalPosition.top, tmp.showCmd, (int)IsManaged(h));

        /* 接管过的窗口: 位置/尺寸一律保持现状。
           实测 (build 04:36 日志): 渲染器会周期性回调 SetWindowPlacement,
           把窗口还原成它缓存下来的"普通位置" —— 也就是我们小窗化时的
           613x321 + 右下角, 于是用户拖大/最大化之后又被拉回去。
           这里把 rcNormalPosition 换成当前矩形, 只保留 showCmd 语义。 */
        if (IsManaged(h)) {
            RECT cr;
            /* 隐藏中的窗口: 允许它重新显示 (否则 Hide 之后再也回不来) */
            if (!IsWindowVisible(h) &&
                (tmp.showCmd == SW_SHOWNORMAL || tmp.showCmd == SW_SHOWMAXIMIZED ||
                 tmp.showCmd == SW_RESTORE)) {
                return real_SetWindowPlacement(h, &tmp);
            }
            /* ★ 已经最小化: 应用发起的任何"显示/还原"都要拦 (用户从任务栏
               恢复是系统操作, 不走这里), 否则 SetWindowPlacement 会把它拉回来。 */
            if (IsIconic(h) &&
                (tmp.showCmd == SW_SHOW || tmp.showCmd == SW_SHOWNORMAL ||
                 tmp.showCmd == SW_RESTORE || tmp.showCmd == SW_SHOWMAXIMIZED ||
                 tmp.showCmd == SW_MAXIMIZE || tmp.showCmd == SW_SHOWNOACTIVATE ||
                 tmp.showCmd == SW_SHOWNA || tmp.showCmd == SW_SHOWDEFAULT)) {
                if (InterlockedIncrement(&g_setwpLogs) <= 40)
                    HookLog("ignore app show hwnd=%p (SetWindowPlacement, minimized)",
                            (void*)h);
                return TRUE;
            }

            /* 渲染器自己发起的"还原" (用户没下命令) -> 保持现状 */
            if (!UserCmdRecent(h, 1500) &&
                (tmp.showCmd == SW_SHOWNORMAL || tmp.showCmd == SW_RESTORE ||
                 tmp.showCmd == SW_SHOW ||
                 tmp.showCmd == SW_SHOWMAXIMIZED || tmp.showCmd == SW_MAXIMIZE)) {
                if (InterlockedIncrement(&g_setwpLogs) <= 40)
                    HookLog("keep user state hwnd=%p (app SetWindowPlacement show=%d)",
                            (void*)h, tmp.showCmd);
                return TRUE;
            }
            /* 应用发起的"最小化"同样拦掉: 广播窗最小化 = 画面消失 */
            if (!UserCmdRecent(h, 1500) &&
                (tmp.showCmd == SW_SHOWMINIMIZED || tmp.showCmd == SW_MINIMIZE ||
                 tmp.showCmd == SW_SHOWMINNOACTIVE || tmp.showCmd == SW_FORCEMINIMIZE)) {
                if (InterlockedIncrement(&g_setwpLogs) <= 40)
                    HookLog("ignore app minimize hwnd=%p (SetWindowPlacement show=%d)",
                            (void*)h, tmp.showCmd);
                return TRUE;
            }
            /* ⚠ 最小化时 GetWindowRect 返回的是 (-32000,-32000) 附近的幽灵矩形,
               写进 rcNormalPosition 会让窗口还原到屏幕外。最小化时保持原值。
               最大化时同理: 量到的是满屏矩形, 写进去会让"还原"变成满屏。
               所以优先用我们记录的"用户矩形"。 */
            if (!IsIconic(h)) {
                RECT ur;
                if (GetUserRect(h, &ur)) tmp.rcNormalPosition = ur;
                else if (!IsZoomed(h) && GetWindowRect(h, &cr))
                    tmp.rcNormalPosition = cr;
            }
            return real_SetWindowPlacement(h, &tmp);
        }

        nx = g_smallX; ny = g_smallY; nw = g_smallW; nh = g_smallH;
        if (tmp.showCmd == SW_SHOWMAXIMIZED || tmp.showCmd == SW_MAXIMIZE)
            tmp.showCmd = SW_SHOWNORMAL;
        rqW = tmp.rcNormalPosition.right - tmp.rcNormalPosition.left;
        rqH = tmp.rcNormalPosition.bottom - tmp.rcNormalPosition.top;
        if (rqW > 0 && rqH > 0) SmallRectFor(rqW, rqH, &nx, &ny, &nw, &nh);
        tmp.rcNormalPosition.left   = nx;
        tmp.rcNormalPosition.top    = ny;
        tmp.rcNormalPosition.right  = nx + nw;
        tmp.rcNormalPosition.bottom = ny + nh;
        return real_SetWindowPlacement(h, &tmp);
    }
    return real_SetWindowPlacement(h, wp);
}

static BOOL ShowStateAllowed(HWND h, int cmd)
{
    /* ★ 已"收起"(parked): 应用的一切显示/还原/最大化请求都拦掉, 只允许隐藏 */
    if (GetPropA(h, PROP_PARKED)) {
        if (cmd != SW_HIDE) {
            if (InterlockedIncrement(&g_showLogs) <= 24)
                HookLog("keep parked hwnd=%p (ignore app show cmd=%d)", (void*)h, cmd);
            return FALSE;
        }
        return TRUE;
    }

    /* ★ 任务栏最小化锁: 只要是我们主动最小化到任务栏的窗口, 应用发起的
       所有显示/还原/最大化请求一律拦掉 —— 不再依赖 IsWindowVisible/IsIconic
       这些"可能已经被应用改过"的状态位 (ShowWindow 会先清 WS_MINIMIZE,
       等我们判 IsIconic 时已经晚了)。用户从任务栏恢复是系统在别的进程里
       直接调 win32u, 不经过这个钩子, 不受影响。 */
    if (TaskbarMinimized(h)) {
        if (cmd != SW_HIDE) {
            if (InterlockedIncrement(&g_showLogs) <= 24)
                HookLog("ignore app show on taskbar-minimized hwnd=%p (cmd=%d)",
                        (void*)h, cmd);
            return FALSE;
        }
        return TRUE;
    }

    /* 已接管的广播窗: 应用发起的"最大化/还原/普通显示"一律忽略。
       用户想要的动作 (点按钮/双击标题栏) 由 SubclassProc 里的
       SC_MAXIMIZE / SC_RESTORE 处理器直接执行, 不再需要放行应用的调用。
       这样就不存在"宽限期内被应用钻空子"的问题了。 */
    if (!IsBroadcastWindow(h) || !LooksManaged(h)) return TRUE;

    /* ⚠ 例外: 窗口当前是隐藏的 (应用自己 Hide 过, 或者刚被最小化),
       必须允许它重新显示 —— 否则一旦 Hide 就再也回不来了。 */
    if (!IsWindowVisible(h) && cmd != SW_HIDE) return TRUE;

    /* ★ 窗口已经最小化时, 应用发起的 SW_SHOW / SW_SHOWNORMAL 也会把它
       "还原" (实测 ScreenRender 周期性 ShowWindow(SW_SHOW), 产生
       flags=0x8120 的状态变更) —— 这就是"点最小化又被拉回最大化".
       用户从任务栏恢复是系统内部操作, 不经过这个 IAT 钩子, 不受影响。 */
    if (IsIconic(h) && (cmd == SW_SHOW || cmd == SW_SHOWNORMAL ||
                        cmd == SW_RESTORE || cmd == SW_SHOWMAXIMIZED ||
                        cmd == SW_MAXIMIZE || cmd == SW_SHOWNOACTIVATE ||
                        cmd == SW_SHOWNA || cmd == SW_SHOWDEFAULT)) {
        if (InterlockedIncrement(&g_showLogs) <= 24)
            HookLog("ignore app show on minimized hwnd=%p (cmd=%d)", (void*)h, cmd);
        return FALSE;
    }

    if (cmd == SW_RESTORE || cmd == SW_SHOWNORMAL ||
        cmd == SW_SHOWMAXIMIZED || cmd == SW_MAXIMIZE) {
        if (InterlockedIncrement(&g_showLogs) <= 24)
            HookLog("ignore app window-state change hwnd=%p (cmd=%d)", (void*)h, cmd);
        return FALSE;
    }

    /* ★ 应用发起的"最小化"也要拦: 广播窗被最小化 = 画面直接消失,
       只能去任务栏捞回来 (实测 ScreenRender 会周期性这么做, 看起来就是
       "窗口反复横跳")。用户点最小化按钮走 WM_SYSCOMMAND, 在 SubclassProc
       里单独处理, 不经过这里。 */
    if (cmd == SW_MINIMIZE || cmd == SW_SHOWMINIMIZED ||
        cmd == SW_SHOWMINNOACTIVE || cmd == SW_FORCEMINIMIZE) {
        if (InterlockedIncrement(&g_showLogs) <= 24)
            HookLog("ignore app minimize hwnd=%p (cmd=%d)", (void*)h, cmd);
        return FALSE;
    }
    return TRUE;
}

static BOOL WINAPI Hook_ShowWindow(HWND h, int cmd)
{
    if (!g_disabled && IsTargetWindow(h) &&
        cmd != SW_SHOW && cmd != SW_SHOWNORMAL &&
        InterlockedIncrement(&g_showLogs) <= 20)
        HookLog("showwin hwnd=%p cmd=%d", (void*)h, cmd);

    /* 接管过的窗口: 最大化/还原/普通显示 都必须由用户发起。
       渲染器自己每 500ms 调一次 SW_RESTORE (日志实证), 不退它的;
       真正点按钮/双击标题栏会先收到 WM_SYSCOMMAND, 那时才放行。
       ⚠ ShowWindowAsync 必须一起拦: 实测渲染器走的正是这条异步路径,
         只拦 ShowWindow 的话, 窗口尺寸被冻住了但 WS_MAXIMIZE 已经被它清掉,
         于是变成"看着是满屏、其实不是最大化", 再点还原就毫无反应。 */
    if (!g_disabled && IsTargetWindow(h) && !ShowStateAllowed(h, cmd))
        return TRUE;

    /* ★ 广播窗口不允许被自己隐藏 (cmd=0/SW_HIDE)。
       实测 ScreenRender 在重建/切换时会 ShowWindow(SW_HIDE), 然后依赖下一帧
       再 Show; 一旦我们正在改写它的几何, 它就停在被隐藏的状态 ——
       用户看到的就是"广播窗透明/没了"。 */
    if (!g_disabled && cmd == SW_HIDE && IsTargetWindow(h) &&
        (IsBroadcastWindow(h) || IsManaged(h))) {
        if (InterlockedIncrement(&g_showLogs) <= 20)
            HookLog("suppressed ShowWindow(SW_HIDE) hwnd=%p (broadcast window)", (void*)h);
        return TRUE;
    }

    if (!g_disabled && IsTargetWindow(h) && IsBroadcastWindow(h) && !LooksManaged(h) &&
        (cmd == SW_SHOWMAXIMIZED || cmd == SW_MAXIMIZE))
        cmd = SW_SHOWNORMAL;

    /* virtual desktop: pin before the window becomes visible */
    if (!g_disabled)
        VD_PinNow(h, "showwindow");

    /* Showing a window that lives on a virtual desktop the user is NOT looking
       at makes the shell switch to that desktop - that is exactly the
       "re-broadcast drags me from desktop 2 back to desktop 1" bug.
       Such show requests lose their activate semantics only: the window still
       shows up, the user's desktop does not move. */
    if (!g_disabled && VD_NoActivate(h) &&
        (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_SHOWDEFAULT ||
         cmd == SW_RESTORE || cmd == SW_SHOWMAXIMIZED || cmd == SW_MAXIMIZE)) {
        if (VD_LogWanted(h, VD_LOG_SHOW, VD_LOG_MS))
            HookLog("vd: ShowWindow(%d) -> SW_SHOWNOACTIVATE hwnd=%p "
                    "(pinned desktop is not current)", cmd, (void*)h);
        cmd = SW_SHOWNOACTIVATE;
    }
    return real_ShowWindow(h, cmd);
}

static BOOL WINAPI Hook_ShowWindowAsync(HWND h, int cmd)
{
    if (!g_disabled && IsTargetWindow(h) && cmd != SW_SHOW && cmd != SW_SHOWNORMAL &&
        InterlockedIncrement(&g_showLogs) <= 20)
        HookLog("showwinasync hwnd=%p cmd=%d", (void*)h, cmd);

    /* 和 ShowWindow 一样: 广播窗不允许被隐藏 */
    if (!g_disabled && cmd == SW_HIDE && IsTargetWindow(h) &&
        (IsBroadcastWindow(h) || IsManaged(h))) {
        if (InterlockedIncrement(&g_showLogs) <= 20)
            HookLog("suppressed ShowWindowAsync(SW_HIDE) hwnd=%p", (void*)h);
        return TRUE;
    }

    if (!g_disabled && IsTargetWindow(h) && !ShowStateAllowed(h, cmd))
        return TRUE;
    if (!real_ShowWindowAsync) return TRUE;
    if (!g_disabled)
        VD_PinNow(h, "showwindowasync");

    /* same as ShowWindow: no activation when the window is on another desktop */
    if (!g_disabled && VD_NoActivate(h) &&
        (cmd == SW_SHOW || cmd == SW_SHOWNORMAL || cmd == SW_SHOWDEFAULT ||
         cmd == SW_RESTORE || cmd == SW_SHOWMAXIMIZED || cmd == SW_MAXIMIZE)) {
        if (VD_LogWanted(h, VD_LOG_SHOWASYNC, VD_LOG_MS))
            HookLog("vd: ShowWindowAsync(%d) -> SW_SHOWNOACTIVATE hwnd=%p "
                    "(pinned desktop is not current)", cmd, (void*)h);
        cmd = SW_SHOWNOACTIVATE;
    }
    return real_ShowWindowAsync(h, cmd);
}

/* 渲染器会 ShowCursor(0) 把光标藏掉; 直接无视隐藏请求 */
static int WINAPI Hook_ShowCursor(BOOL show)
{
    if (!g_disabled && !show) return real_ShowCursor(TRUE);
    return real_ShowCursor(show);
}

/* SDL 在窗口里会 SetCursor(NULL) (相当于把光标藏起来, 但 ShowCursor 计数不变),
   鼠标一进小窗光标就"消失"就是它干的。这里换成一个箭头。 */
static HCURSOR WINAPI Hook_SetCursor(HCURSOR c)
{
    if (!g_disabled && !c) {
        HCURSOR arrow = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
        if (arrow) return real_SetCursor(arrow);
    }
    return real_SetCursor(c);
}

/* 桌面名 (诊断用) */
static void DesktopName(char* out, int n)
{
    HDESK d;
    char  buf[128] = "?";

    out[0] = '?'; out[1] = '\0';
    d = GetThreadDesktop(GetCurrentThreadId());
    if (!d) return;
    if (GetUserObjectInformationA(d, 2 /*UOI_NAME*/, buf, sizeof(buf), NULL))
        _snprintf(out, n - 1, "%s", buf);
    out[n - 1] = '\0';
}

/* 窗口模式 = "把全屏广播降级成普通窗口并留在学生自己的桌面上"。
   ★ 默认开启，由 bypass_config.ini 的 window_mode=0/1 控制；
     主程序写共享内存，DLL 通过事件/轮询同步。 */
static BOOL WindowModeOn(void)
{
    return g_stWindow != 0;
}

/* 切桌面: 一旦拦下来, 进程就一直留在原来的桌面 (学生自己的桌面)。
   证据: MultiClient.exe 的 tcpclient 构造里 CreateDesktopA("NetTeachingDesktop"),
   再由 FUN_00402c90 在启动 ScreenRender 之后调 SwitchDesktop(hdesk)。 */
static BOOL WINAPI Hook_SwitchDesktop(HDESK desk)
{
    if (!g_disabled &&
        WindowModeOn()) {
        if (InterlockedIncrement(&g_deskLogs) <= 8)
            HookLog("blocked SwitchDesktop(%p) - staying on current desktop", (void*)desk);
        return TRUE;      /* 假装成功 */
    }
    if (!real_SwitchDesktop) return TRUE;
    return real_SwitchDesktop(desk);
}

/* 全局低级鼠标/键盘钩子: 课堂客户端装 WH_MOUSE_LL / WH_KEYBOARD_LL,
   把 dwExtraInfo 不是 0x12452 的事件全部丢掉 —— 也就是真实鼠标键盘全废。
   留在自己的桌面上时, 这个钩子就是"看得见却点不动"的原因。
   窗口模式开启时, 直接不安装 (返回一个假句柄), 让它以为自己成功了。 */
static HHOOK WINAPI Hook_SetWindowsHookExA(int id, HOOKPROC fn, HINSTANCE h, DWORD tid){
    if (!g_disabled &&
        WindowModeOn() &&
        (id == WH_MOUSE_LL || id == WH_KEYBOARD_LL)) {
        if (InterlockedIncrement(&g_hookLogs) <= 8)
            HookLog("suppressed SetWindowsHookExA(id=%d) - input stays live", id);
        return (HHOOK)(ULONG_PTR)0x1;
    }
    if (!real_SetWindowsHookExA) return NULL;
    return real_SetWindowsHookExA(id, fn, h, tid);
}

static HHOOK WINAPI Hook_SetWindowsHookExW(int id, HOOKPROC fn, HINSTANCE h, DWORD tid)
{
    if (!g_disabled &&
        WindowModeOn() &&
        (id == WH_MOUSE_LL || id == WH_KEYBOARD_LL)) {
        if (InterlockedIncrement(&g_hookLogs) <= 8)
            HookLog("suppressed SetWindowsHookExW(id=%d) - input stays live", id);
        return (HHOOK)(ULONG_PTR)0x1;
    }
    if (!real_SetWindowsHookExW) return NULL;
    return real_SetWindowsHookExW(id, fn, h, tid);
}

/* 拉起子进程时被显式指定 lpDesktop="NetTeachingDesktop":
   拦 SwitchDesktop 管不到这里 (子进程会被直接建在隐藏桌面上,
   于是"留在自己桌面"的那个渲染器拿不到画面, 看起来就像透明窗口)。
   窗口模式开启时把 lpDesktop 清空 -> 子进程继承本进程的桌面。
   ⚠ 只动教学桌面这一个目标, 不要把别的子进程(浏览器/资源管理器…)也从
   它们自己指定的桌面上拽下来。 */
static int IsTeachingDesktop(const char* d)
{
    if (!d) return 0;
    return _stricmp(d, "NetTeachingDesktop") == 0 ? 1 : 0;
}

/* 把命令行里的 fullscreen 参数改成 0 —— 让渲染器走"窗口模式"分支:
   0xC40000 (带 WS_THICKFRAME) + 3/4 屏。这样 SDL 视窗口为 resizable,
   不会在 WM_GETMINMAXINFO 里把尺寸钉死, 于是任何进程都能把它改小,
   不再依赖"渲染器是否被及时注入"。 */
static char* ForceWindowModeArg(char* cmd, char* buf, size_t n)
{
    char* p;

    if (!cmd || !buf || n < 8) return cmd;
    /* ⚠ 超长就放弃改写: 截断后照传会让子进程命令行损坏 (丢参数/路径截半) */
    if (strlen(cmd) >= n) return cmd;
    if (!strstr(cmd, "fullscreen")) return cmd;

    _snprintf(buf, n - 1, "%s", cmd);
    buf[n - 1] = '\0';

    p = strstr(buf, "fullscreen");
    while (p) {
        char* q = p + 10;                 /* strlen("fullscreen") */
        /* token 边界: 前一个字符不能是字母/数字/路径分隔符,
           否则像 C:\tools\fullscreen\render.exe 这种路径段会被误改 */
        char prev = (p == buf) ? '\0' : p[-1];
        int  atToken = !( (prev >= 'a' && prev <= 'z') ||
                          (prev >= 'A' && prev <= 'Z') ||
                          (prev >= '0' && prev <= '9') ||
                          prev == '_' || prev == '\\' || prev == '/');

        if (atToken) {
            while (*q && *q != ':' && *q != '=') q++;
            if ((*q == ':' || *q == '=') && q[1] == '1' && q[2] != '0') {
                q[1] = '0';
                return buf;
            }
        }
        p = strstr(p + 10, "fullscreen");
    }
    return cmd;
}

/* 父进程把"这个子进程的 fullscreen:1 是我们改成 0 的"写成一个按 PID 命名的
   小标记文件, 子进程在 Worker 里读掉并删除。
   为什么不用环境变量: 应用可能给 CreateProcess 传自定义环境块, 那种情况下
   SetEnvironmentVariable 不会影响子进程 —— 实测同一个教师命令, 有的
   ScreenRender 拿到了 BYPASS_BROADCAST、有的没拿到, 表现就是"有时缩、有时不缩".
   挂起注入保证标记文件在子进程主线程恢复前就已经写好。 */
static void MarkForcedChild(DWORD pid)
{
    char   path[MAX_PATH];
    char   buf[32];
    char*  p;
    HANDLE f;
    DWORD  wrote = 0;
    int    n;

    if (!pid || !g_dllPath[0]) return;
    _snprintf(path, sizeof(path) - 1, "%s", g_dllPath);
    path[sizeof(path) - 1] = '\0';
    p = strrchr(path, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(path + strlen(path), sizeof(path) - strlen(path) - 1,
              "bypass_forced_%lu.tmp", (unsigned long)pid);
    f = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    /* ★ 写时间戳 (GetTickCount 全系统同一基准): 万一子进程没来得及读就退出,
       残留文件不会被 PID 复用后的另一个渲染器当成"这是被改写过的广播". */
    n = _snprintf(buf, sizeof(buf) - 1, "%lu", (unsigned long)GetTickCount());
    if (n > 0) WriteFile(f, buf, (DWORD)n, &wrote, NULL);
    CloseHandle(f);
}

static BOOL ConsumeForcedMark(void)
{
    char  path[MAX_PATH];
    char  buf[32];
    char* p;
    HANDLE f;
    DWORD rd = 0;
    unsigned long t = 0;

    if (!g_dllPath[0]) return FALSE;
    _snprintf(path, sizeof(path) - 1, "%s", g_dllPath);
    path[sizeof(path) - 1] = '\0';
    p = strrchr(path, '\\');
    if (!p) return FALSE;
    *(p + 1) = '\0';
    _snprintf(path + strlen(path), sizeof(path) - strlen(path) - 1,
              "bypass_forced_%lu.tmp", (unsigned long)g_pid);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return FALSE;

    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        if (ReadFile(f, buf, sizeof(buf) - 1, &rd, NULL) && rd > 0) {
            buf[rd] = '\0';
            t = strtoul(buf, NULL, 10);
        }
        CloseHandle(f);
    }
    DeleteFileA(path);
    if (!t) return FALSE;                       /* 旧格式/损坏: 不认 */
    return ((DWORD)(GetTickCount() - (DWORD)t) < 60000) ? TRUE : FALSE;
}

static BOOL WINAPI Hook_CreateProcessA(LPCSTR app, LPSTR cmd,
                                       LPSECURITY_ATTRIBUTES pa,
                                       LPSECURITY_ATTRIBUTES ta, BOOL inh,
                                       DWORD flags, LPVOID env, LPCSTR cwd,
                                       LPSTARTUPINFOA si,
                                       LPPROCESS_INFORMATION pi)
{
    STARTUPINFOA tmp;
    char         cmdbuf[2048];
    BOOL         isBroadcast = FALSE;
    BOOL         ok;

    if (!g_disabled && WindowModeOn()) {
        const char* old = cmd;
        cmd = ForceWindowModeArg(cmd, cmdbuf, sizeof(cmdbuf));
        if (cmd != old) {
            isBroadcast = TRUE;
            if (InterlockedIncrement(&g_procLogs) <= 8)
                HookLog("forced window mode in cmd: %s", cmd);
        }
    }

    if (!g_disabled && si && si->lpDesktop && IsTeachingDesktop(si->lpDesktop) &&
        WindowModeOn()) {
        if (InterlockedIncrement(&g_procLogs) <= 8)
            HookLog("cleared lpDesktop='%s' for cmd='%s'",
                    si->lpDesktop, cmd ? cmd : (app ? app : "?"));
        tmp = *si;
        tmp.lpDesktop = NULL;
        si = &tmp;
    }
    if (!real_CreateProcessA) return FALSE;

    /* ★ 给这个子进程留一份"我是全屏广播渲染器"的身份证。
       命令行里的 fullscreen:1 已经被改写成 0, 子进程自己就看不出来了;
       环境变量在参数解析之外, 不会被渲染器当成参数, 最安全。
       子进程里的 DLL 启动时读 BYPASS_BROADCAST=1 -> 不再用尺寸猜窗口。 */
    if (isBroadcast && !env) SetEnvironmentVariableA("BYPASS_BROADCAST", "1");

    /* ★ 挂起注入: 先建进程但不让它跑, 注入 DLL, 再恢复。
       这样 MultiClient 的 SwitchDesktop 一定被钩住。
       调用方自己传了 CREATE_SUSPENDED 时也要注入, 但由它自己恢复。 */
    {
        BOOL suspendedByUs = FALSE;
        BOOL needInject    = FALSE;

        if (!g_disabled && pi && IsInjectAtBirth(app, cmd)) {
            needInject = TRUE;
            if (!(flags & CREATE_SUSPENDED)) {
                flags |= CREATE_SUSPENDED;
                suspendedByUs = TRUE;
            }
        }
        ok = real_CreateProcessA(app, cmd, pa, ta, inh, flags, env, cwd, si, pi);
        if (ok && needInject && pi) {
            if (isBroadcast) MarkForcedChild(pi->dwProcessId);
            TryInjectOwner(pi->dwProcessId);
            if (suspendedByUs) ResumeThread(pi->hThread);
            HookLog("inject-at-birth pid=%lu (%s)%s",
                    (unsigned long)pi->dwProcessId,
                    app ? app : "cmdline",
                    suspendedByUs ? "" : " [caller-suspended]");
        }
    }

    if (isBroadcast && !env) SetEnvironmentVariableA("BYPASS_BROADCAST", NULL);
    return ok;
}

static BOOL WINAPI Hook_CreateProcessW(LPCWSTR app, LPWSTR cmd,
                                       LPSECURITY_ATTRIBUTES pa,
                                       LPSECURITY_ATTRIBUTES ta, BOOL inh,
                                       DWORD flags, LPVOID env, LPCWSTR cwd,
                                       LPSTARTUPINFOW si,
                                       LPPROCESS_INFORMATION pi)
{
    STARTUPINFOW tmp;
    WCHAR        cmdbuf[2048];
    BOOL         isBroadcast = FALSE;
    BOOL         ok;

    if (!g_disabled && WindowModeOn() &&
        cmd && wcsstr(cmd, L"fullscreen") && wcslen(cmd) < 2048) {
        WCHAR* p;

        wcsncpy(cmdbuf, cmd, 2047);
        cmdbuf[2047] = 0;
        p = wcsstr(cmdbuf, L"fullscreen");
        while (p) {
            WCHAR* q = p + 10;
            WCHAR  prev = (p == cmdbuf) ? 0 : p[-1];
            BOOL   atToken = !(((prev >= L'a' && prev <= L'z') ||
                                (prev >= L'A' && prev <= L'Z') ||
                                (prev >= L'0' && prev <= L'9')) ||
                               prev == L'_' || prev == L'\\' || prev == L'/');

            while (atToken && *q && *q != L':' && *q != L'=') q++;
            if (atToken && (*q == L':' || *q == L'=') &&
                q[1] == L'1' && q[2] != L'0') {
                q[1] = L'0';
                cmd = cmdbuf;
                isBroadcast = TRUE;
                if (InterlockedIncrement(&g_procLogs) <= 8)
                    HookLog("forced window mode in cmd(W)");
                break;
            }
            p = wcsstr(p + 10, L"fullscreen");
        }
    }

    if (!g_disabled && si && si->lpDesktop && si->lpDesktop[0] &&
        WindowModeOn()) {
        char narrow[128];
        narrow[0] = '\0';                    /* 转换失败时不能读未初始化栈 */
        WideCharToMultiByte(CP_ACP, 0, si->lpDesktop, -1, narrow,
                            sizeof(narrow) - 1, NULL, NULL);
        narrow[sizeof(narrow) - 1] = '\0';
        if (IsTeachingDesktop(narrow)) {
            if (InterlockedIncrement(&g_procLogs) <= 8)
                HookLog("cleared lpDesktop(W)='%s'", narrow);
            tmp = *si;
            tmp.lpDesktop = NULL;
            si = &tmp;
        }
    }
    if (!real_CreateProcessW) return FALSE;

    /* 同 A 版: 给子进程留"广播渲染器"身份证 (环境变量, 不进参数) */
    if (isBroadcast && !env) SetEnvironmentVariableA("BYPASS_BROADCAST", "1");

    /* 同 A 版: 挂起 -> 注入 -> 恢复 (app 和 cmd 都要看, 路径可能带空格) */
    {
        BOOL        suspendedByUs = FALSE;
        BOOL        needInject    = FALSE;
        char        narrowCmd[1024];
        char        narrowApp[1024];
        const char* cmdA = NULL;
        const char* appA = NULL;

        if (cmd && cmd[0]) {
            narrowCmd[0] = '\0';
            WideCharToMultiByte(CP_ACP, 0, cmd, -1, narrowCmd,
                                sizeof(narrowCmd) - 1, NULL, NULL);
            narrowCmd[sizeof(narrowCmd) - 1] = '\0';
            cmdA = narrowCmd;
        }
        if (app && app[0]) {
            narrowApp[0] = '\0';
            WideCharToMultiByte(CP_ACP, 0, app, -1, narrowApp,
                                sizeof(narrowApp) - 1, NULL, NULL);
            narrowApp[sizeof(narrowApp) - 1] = '\0';
            appA = narrowApp;
        }

        if (!g_disabled && pi && IsInjectAtBirth(appA, cmdA)) {
            needInject = TRUE;
            if (!(flags & CREATE_SUSPENDED)) {
                flags |= CREATE_SUSPENDED;
                suspendedByUs = TRUE;
            }
        }
        ok = real_CreateProcessW(app, cmd, pa, ta, inh, flags, env, cwd, si, pi);
        if (ok && needInject && pi) {
            if (isBroadcast) MarkForcedChild(pi->dwProcessId);
            TryInjectOwner(pi->dwProcessId);
            if (suspendedByUs) ResumeThread(pi->hThread);
            HookLog("inject-at-birth(W) pid=%lu%s",
                    (unsigned long)pi->dwProcessId,
                    suspendedByUs ? "" : " [caller-suspended]");
        }
    }

    if (isBroadcast && !env) SetEnvironmentVariableA("BYPASS_BROADCAST", NULL);
    return ok;
}

/* ------------------------------------------------------------------ */
/* IAT 打补丁                                                          */
/* ------------------------------------------------------------------ */

static void PatchOne(HMODULE mod, const char* dllName,
                     const char* fnName, void* hook)
{    IMAGE_DOS_HEADER*        dos;
    IMAGE_NT_HEADERS*        nt;
    IMAGE_IMPORT_DESCRIPTOR* imp;
    DWORD                    rva;

    if (!mod || (HMODULE)mod == g_self) return;

    dos = (IMAGE_DOS_HEADER*)mod;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;

    nt = (IMAGE_NT_HEADERS*)((BYTE*)mod + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;

    imp = (IMAGE_IMPORT_DESCRIPTOR*)((BYTE*)mod + rva);
    for (; imp->Name; imp++) {
        const char*           name = (const char*)((BYTE*)mod + imp->Name);
        IMAGE_THUNK_DATA*     oft;
        IMAGE_THUNK_DATA*     ft;

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

            if (ft->u1.Function == (DWORD_PTR)hook) continue;   /* 已打过 */

            if (VirtualProtect(&ft->u1.Function, sizeof(DWORD_PTR),
                               PAGE_READWRITE, &old)) {
                ft->u1.Function = (DWORD_PTR)hook;
                VirtualProtect(&ft->u1.Function, sizeof(DWORD_PTR), old, &old);
                g_patchCount++;
            }
        }
    }
}

static void PatchAllModules(void)
{
    HMODULE mods[512];
    DWORD   needed = 0, i;
    HANDLE  hp = GetCurrentProcess();

    if (!EnumProcessModules(hp, mods, sizeof(mods), &needed)) return;
    if (needed > sizeof(mods)) needed = sizeof(mods);

    for (i = 0; i < needed / sizeof(HMODULE); i++) {
        PatchOne(mods[i], "user32.dll", "SetWindowPos",        (void*)Hook_SetWindowPos);
        PatchOne(mods[i], "user32.dll", "SetWindowLongW",      (void*)Hook_SetWindowLongW);
        PatchOne(mods[i], "user32.dll", "SetWindowLongA",      (void*)Hook_SetWindowLongA);
        PatchOne(mods[i], "user32.dll", "SetWindowLongPtrW",   (void*)Hook_SetWindowLongPtrW);
        PatchOne(mods[i], "user32.dll", "SetWindowLongPtrA",   (void*)Hook_SetWindowLongPtrA);
        PatchOne(mods[i], "user32.dll", "MoveWindow",          (void*)Hook_MoveWindow);
        PatchOne(mods[i], "user32.dll", "SetWindowPlacement",  (void*)Hook_SetWindowPlacement);
        PatchOne(mods[i], "user32.dll", "ShowWindow",          (void*)Hook_ShowWindow);
        PatchOne(mods[i], "user32.dll", "ShowWindowAsync",     (void*)Hook_ShowWindowAsync);
        PatchOne(mods[i], "user32.dll", "ShowCursor",          (void*)Hook_ShowCursor);
        PatchOne(mods[i], "user32.dll", "SetCursor",           (void*)Hook_SetCursor);
        PatchOne(mods[i], "user32.dll", "SwitchDesktop",       (void*)Hook_SwitchDesktop);
        PatchOne(mods[i], "user32.dll", "SetWindowsHookExA",   (void*)Hook_SetWindowsHookExA);
        PatchOne(mods[i], "user32.dll", "SetWindowsHookExW",   (void*)Hook_SetWindowsHookExW);
        PatchOne(mods[i], "kernel32.dll", "CreateProcessA",    (void*)Hook_CreateProcessA);
        PatchOne(mods[i], "kernel32.dll", "CreateProcessW",    (void*)Hook_CreateProcessW);
        PatchOne(mods[i], "user32.dll", "CreateWindowExW",     (void*)Hook_CreateWindowExW);
        PatchOne(mods[i], "user32.dll", "SetForegroundWindow", (void*)Hook_SetForegroundWindow);
        PatchOne(mods[i], "user32.dll", "BringWindowToTop",    (void*)Hook_BringWindowToTop);
        PatchOne(mods[i], "user32.dll", "SetActiveWindow",     (void*)Hook_SetActiveWindow);
    }
}

/* ------------------------------------------------------------------ */
/* 保险丝: 每 1s 兜一次底 (只在"真的变大/置顶"时才动手, 不会闪)          */
/* ------------------------------------------------------------------ */

/* 跨进程兜底次数: 同一个窗口最多帮 3 次, 之后交给窗口所有者自己的钩子。
   实测: 渲染器进程内的子类化一定能改成功, 别的进程反复抢只会互相打架
   (日志里那一串 "!! readback mismatch ... 1792x850" 就是这么来的)。 */
#define MAX_CROSS 8
static struct { HWND h; int tries; } g_cross[MAX_CROSS];

/* 自己把自己注进"还没有钩子"的课堂进程里 (32 位 -> 32 位)。
   为什么需要: 渲染器是广播开始时才被拉起来的, 主程序的定时器慢一拍,
   这一拍里窗口就是满屏, 而且只有所有者进程的钩子能把尺寸改下来。 */
/* 目标进程里是不是已经有我们了 (避免重复自注入) */
static BOOL HasOurDll(DWORD pid)
{
    HANDLE snap;
    MODULEENTRY32 me;
    BOOL  found = FALSE;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (!_stricmp(me.szModule, "bypass_hook32.dll")) {
                found = TRUE;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

static void TryInjectOwner(DWORD pid)
{
    static struct { DWORD pid; DWORD t; int n; } seen[8];
    static int   si;    HMODULE      k32;
    HANDLE       hp, th;
    void*        mem;
    SIZE_T       bytes;
    DWORD        written = 0;
    int          i;

    if (!pid || pid == g_pid || !g_dllPath[0]) return;
    if (PidHasDll(pid)) return;              /* 已经有了, 不用再注 */

    for (i = 0; i < 8; i++) {
        if (seen[i].pid == pid) {
            if (seen[i].n >= 3) return;
            if (GetTickCount() - seen[i].t < 3000) return;   /* 3 秒内不重复 */
            seen[i].t = GetTickCount();
            seen[i].n++;
            goto do_it;
        }
    }
    seen[si].pid = pid;
    seen[si].t   = GetTickCount();
    seen[si].n   = 1;
    si = (si + 1) % 8;

do_it:
    k32 = GetModuleHandleA("kernel32.dll");
    if (!k32) return;

    hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                     PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION,
                     FALSE, pid);
    if (!hp) {
        HookLog("self-inject pid=%lu failed: OpenProcess err=%lu",
                (unsigned long)pid, (unsigned long)GetLastError());
        return;
    }

    /* 目标进程里要的是宽字符路径: 本 DLL 路径来自 GetModuleFileNameA */
    {
        WCHAR wpath[MAX_PATH];
        if (!MultiByteToWideChar(CP_ACP, 0, g_dllPath, -1, wpath, MAX_PATH)) {
            CloseHandle(hp);
            return;
        }
        bytes = (wcslen(wpath) + 1) * sizeof(WCHAR);
        mem = VirtualAllocEx(hp, NULL, bytes, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE);
        if (!mem ||
            !WriteProcessMemory(hp, mem, wpath, bytes, &written) ||
            written != bytes) {
            HookLog("self-inject pid=%lu failed: write err=%lu",
                    (unsigned long)pid, (unsigned long)GetLastError());
            if (mem) VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
            CloseHandle(hp);
            return;
        }

        th = CreateRemoteThread(hp, NULL, 0,
                                (LPTHREAD_START_ROUTINE)GetProcAddress(k32, "LoadLibraryW"),
                                mem, 0, NULL);
        if (!th) {
            HookLog("self-inject pid=%lu failed: CreateRemoteThread err=%lu",
                    (unsigned long)pid, (unsigned long)GetLastError());
            VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
            CloseHandle(hp);
            return;
        }
        if (WaitForSingleObject(th, 2000) != WAIT_OBJECT_0) {
            /* 超时: 远程线程可能还在读这块路径内存, 不能释放 (宁可泄漏)。 */
            HookLog("self-inject pid=%lu timeout (path buffer kept)",
                    (unsigned long)pid);
            CloseHandle(th);
            CloseHandle(hp);
            return;
        }
        {
            DWORD code = 0;
            GetExitCodeThread(th, &code);
            HookLog("self-inject pid=%lu -> ret=0x%lX (%s)",
                    (unsigned long)pid, (unsigned long)code,
                    code ? "ok" : "failed");
        }
        CloseHandle(th);
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
    }
    CloseHandle(hp);
}

static BOOL CrossBudget(HWND h)
{
    int i, free = -1;

    for (i = 0; i < MAX_CROSS; i++) {
        if (g_cross[i].h == h) {
            if (g_cross[i].tries >= 3) return FALSE;
            g_cross[i].tries++;
            return TRUE;
        }
        /* ⚠ 这里必须是 free == -1: 旧写法 !free 等价于 free == 0,
           -1 永远不等于 0, 空槽永远记不下来, 整张表恒为空。 */
        if (free == -1 && (!g_cross[i].h || !IsWindow(g_cross[i].h))) free = i;
    }
    if (free >= 0) { g_cross[free].h = h; g_cross[free].tries = 1; }
    return TRUE;
}

/* 已经被我们小窗化过的窗口: 记住它, 之后只管样式(去置顶/去 POPUP),
   不再动尺寸 —— 否则用户拖大、最大化会被我们一秒一次地拽回去。 */
#define MAX_MANAGED 32
static HWND g_managed[MAX_MANAGED];

/* 已经被我们小窗化过的窗口: 用**窗口属性**做标记 (SetProp/GetProp),
   它是挂在窗口对象上的, 任何进程都读得到 —— 比"进程内表"和
   "看样式像不像"都可靠。
   ⚠ 千万不能用样式判断: 窗口模式下的广播窗天生就是 0x14C40000
   (带 WS_CAPTION、无 WS_POPUP), 会被误判成"已经接管过"而永远不小窗化。 */
#define PROP_SMALL "BypassSmall"

static BOOL IsManaged(HWND h)
{
    int i;

    if (GetPropA(h, PROP_SMALL)) return TRUE;

    /* 进程内小缓存: 句柄会被复用, 必须验证还活着, 否则新窗口可能被误判。
       ⚠ IsWindow 只能证明"这个句柄现在活着", 不能证明是同一个窗口 ——
       句柄复用后, 注释掉的判断会把 SysShadow / 对话框也当成广播窗。
       所以表里命中的还必须真的是渲染器窗口类。 */
    for (i = 0; i < MAX_MANAGED; i++) {
        if (g_managed[i] == h && IsWindow(h)) {
            char cls[96] = "";
            GetClassNameA(h, cls, sizeof(cls));
            if (IsRenderWindowClass(cls)) return TRUE;
        }
    }
    return FALSE;
}

static void MarkManaged(HWND h)
{
    static volatile LONG next;
    LONG slot;

    SetPropA(h, PROP_SMALL, (HANDLE)1);

    slot = InterlockedIncrement(&next) & (MAX_MANAGED - 1);
    g_managed[slot] = h;
}

static BOOL LooksManaged(HWND h)
{
    return IsManaged(h);
}

/* 在 EnumWindows 回调里绝不做注入 (那里等待远程线程会卡死枚举)。
   只把"需要注入的 pid"排队, 由 Worker 循环在枚举结束后处理。 */

/* "目标进程里有没有我们的钩子" —— 带 5 秒缓存。
   这个查询要 CreateToolhelp32Snapshot, 很贵; 放在每秒的兜底循环里
   反复做会明显拖慢 (3 个进程 × 每个大窗口 × 每 200ms)。 */
static BOOL PidHasDll(DWORD pid)
{
    static struct { DWORD pid; BOOL ok; DWORD t; } c[16];
    static volatile LONG next;
    int i, slot;

    if (!pid) return FALSE;
    for (i = 0; i < 16; i++) {
        if (c[i].pid == pid) {
            if (GetTickCount() - c[i].t < 5000) return c[i].ok;
            c[i].ok = HasOurDll(pid);
            c[i].t  = GetTickCount();
            return c[i].ok;
        }
    }
    slot = (int)(InterlockedIncrement(&next) & 15);
    c[slot].pid = pid;
    c[slot].ok  = HasOurDll(pid);
    c[slot].t   = GetTickCount();
    return c[slot].ok;
}

/* 待注入队列: 一个槽会被后来的 pid 覆盖, 改成 4 槽小队列。 */
#define MAX_PENDING 4
static volatile LONG g_pending[MAX_PENDING];

static void DrainPendingPids(void)
{
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        LONG pid = InterlockedExchange(&g_pending[i], 0);
        if (pid) TryInjectOwner((DWORD)pid);
    }
}

static void QueuePendingPid(DWORD pid)
{
    int i;

    for (i = 0; i < MAX_PENDING; i++) {
        if (g_pending[i] == (LONG)pid) return;
    }
    for (i = 0; i < MAX_PENDING; i++) {
        if (!g_pending[i] && !InterlockedExchange(&g_pending[i], (LONG)pid)) return;
    }
}

/* 每次都记一下目标窗口的矩形; 只要变了就写一行, 不管是谁改的、走没走钩子。
   这是唯一能抓到"绕过我们全部钩子"的还原路径的办法。 */
#define MAX_GEO 16
static struct { HWND h; RECT r; } g_geo[MAX_GEO];
static volatile LONG g_geoLogs;

/* 用户矩形: 用户最后一次"自己"摆好的位置/大小 (拖完、最大化前、降级后)。
   最大化/还原时要靠它, 否则系统会把"最大化时的满屏矩形"记成 normal rect,
   于是一还原就变成满屏 (实测: 还原后 2560x1416)。 */
static struct { HWND h; RECT r; } g_user[MAX_GEO];

static void SetUserRect(HWND h, const RECT* r)
{
    int i, freeSlot = -1;

    if (!h || !r) return;
    for (i = 0; i < MAX_GEO; i++) {
        if (g_user[i].h == h) { g_user[i].r = *r; return; }
        if (freeSlot == -1 && (!g_user[i].h || !IsWindow(g_user[i].h))) freeSlot = i;
    }
    if (freeSlot >= 0) { g_user[freeSlot].h = h; g_user[freeSlot].r = *r; }
}

static BOOL GetUserRect(HWND h, RECT* out)
{
    int i;

    if (!h || !out) return FALSE;
    for (i = 0; i < MAX_GEO; i++) {
        if (g_user[i].h == h &&
            (g_user[i].r.right - g_user[i].r.left) > 0) {
            *out = g_user[i].r;
            return TRUE;
        }
    }
    return FALSE;
}

static void TrackGeo(HWND h, const RECT* r)
{
    int i, freeSlot = -1;
    for (i = 0; i < MAX_GEO; i++) {
        if (g_geo[i].h == h) {
            int ow = g_geo[i].r.right - g_geo[i].r.left;
            int oh = g_geo[i].r.bottom - g_geo[i].r.top;
            int nw = r->right - r->left;
            int nh = r->bottom - r->top;

            if (ow != nw || oh != nh ||
                g_geo[i].r.left != r->left || g_geo[i].r.top != r->top) {
                if (InterlockedIncrement(&g_geoLogs) <= 40)
                    HookLog("geo hwnd=%p %dx%d@(%d,%d) -> %dx%d@(%d,%d) st=0x%08X managed=%d",
                            (void*)h, ow, oh, (int)g_geo[i].r.left, (int)g_geo[i].r.top,
                            nw, nh, (int)r->left, (int)r->top,
                            (unsigned)GetWindowLongPtrW(h, GWL_STYLE),
                            (int)IsManaged(h));
                g_geo[i].r = *r;
            }
            return;
        }
        if (freeSlot == -1 && (!g_geo[i].h || !IsWindow(g_geo[i].h))) freeSlot = i;
    }
    if (freeSlot >= 0) { g_geo[freeSlot].h = h; g_geo[freeSlot].r = *r; }
}

/* 窗口销毁: 清掉几何/用户矩形缓存。不然句柄被系统复用后, 新窗口会拿到
   上一个广播窗的"用户矩形", 还原/最大化就会摆到错误的位置。 */
static void ForgetWindowGeom(HWND h)
{
    int i;

    for (i = 0; i < MAX_GEO; i++) {
        if (g_user[i].h == h) {
            g_user[i].h = NULL;
            memset(&g_user[i].r, 0, sizeof(RECT));
        }
        if (g_geo[i].h == h) {
            g_geo[i].h = NULL;
            memset(&g_geo[i].r, 0, sizeof(RECT));
        }
    }
}

/* 自我扩散: 扫描所有课堂进程, 给"还没有我们"的注入自己。
   为什么需要: 先开小窗化、后启动学生端时, MultiClient.exe 可能在主程序
   注入之前就把桌面切到 NetTeachingDesktop —— 之后窗口就永远建在黑桌面上
   (现象: 只有小窗那块有画面, 其它全黑)。
   有了自我扩散, 只要 Student.exe 这类长期进程先拿到钩子, 它就会在
   MultiClient 刚启动的几秒内把它也注上。 */
static void SpreadSelf(void)
{
    static DWORD lastTick;
    HANDLE snap;
    PROCESSENTRY32 pe;

    if (GetTickCount() - lastTick < 2000) return;
    lastTick = GetTickCount();

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (pe.th32ProcessID == 0 || pe.th32ProcessID == g_pid) continue;
            if (!IsClassroomExe(pe.szExeFile)) continue;
            if (PidHasDll(pe.th32ProcessID)) continue;
            TryInjectOwner(pe.th32ProcessID);
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
}

/* ---------------- 共享状态 / 配置回退 ---------------- */

static char* StateTrim(char* s)
{
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

static void StateParseSmall(const char* value, int* w, int* h, int* permille)
{
    char buf[64];
    char* q;

    *w = *h = *permille = 0;
    if (!value) return;
    _snprintf(buf, sizeof(buf) - 1, "%s", value);
    buf[sizeof(buf) - 1] = '\0';
    q = StateTrim(buf);
    if (!*q) return;

    if (strchr(q, 'x') || strchr(q, 'X')) {
        int a = 0, b = 0;
        char sep = 0;
        if (sscanf(q, "%d%c%d", &a, &sep, &b) == 3 && a >= 160 && b >= 120) {
            *w = a;
            *h = b;
        }
    } else {
        double d = atof(q);
        if (d > 0.05 && d < 1.0) {
            *permille = (int)(d * 1000.0 + 0.5);
        } else if (d >= 160.0 && d < 8192.0) {
            *w = (int)d;
        }
    }
}

/* 读配置。defaults=1 时先填默认值（没有共享内存的兜底）；
   defaults=0 时只覆盖文件里出现的键（共享内存已有值时用）。 */
static BOOL StateReadConfig(BypassState* out, int defaults)
{
    FILE* f;
    char line[256];
    unsigned int show = out->show_seq;
    unsigned int small = out->small_seq;

    if (defaults) {
        memset(out, 0, sizeof(*out));
        out->window_mode = 1;
        out->title_autohide = 1;
        out->minimize_tray = 0;
        show = 0;
        small = 0;
    }

    f = fopen(g_configPath, "r");
    if (!f) {
        if (!defaults) return FALSE;
        out->show_seq = show;
        out->small_seq = small;
        return TRUE;
    }

    while (fgets(line, sizeof(line), f)) {
        char* eq;
        char* key;
        char* value;

        key = StateTrim(line);
        if (*key == '\0' || *key == '#' || *key == ';') continue;
        eq = strchr(key, '=');
        if (!eq) continue;
        *eq = '\0';
        value = StateTrim(eq + 1);
        key = StateTrim(key);

        if (!_stricmp(key, "window_mode")) {
            out->window_mode = atoi(value) ? 1 : 0;
        } else if (!_stricmp(key, "title_autohide")) {
            out->title_autohide = atoi(value) ? 1 : 0;
        } else if (!_stricmp(key, "minimize_mode")) {
            out->minimize_tray = (strstr(value, "tray") != NULL) ? 1 : 0;
        } else if (!_stricmp(key, "small")) {
            StateParseSmall(value, &out->small_w, &out->small_h,
                            &out->small_permille);
        }
    }
    fclose(f);
    out->show_seq = show;
    out->small_seq = small;
    return TRUE;
}

static void StateOpen(void)
{
    if (g_stateView) return;
    if (GetTickCount() < g_stateNextOpen) return;
    g_stateNextOpen = GetTickCount() + 2000;

    g_stateMap = OpenFileMappingA(FILE_MAP_READ, FALSE, BYPASS_STATE_MAP_NAME);
    if (!g_stateMap) return;

    g_stateView = (BypassState*)MapViewOfFile(g_stateMap, FILE_MAP_READ,
                                              0, 0, sizeof(BypassState));
    if (!g_stateView) {
        CloseHandle(g_stateMap);
        g_stateMap = NULL;
        return;
    }
    if (g_stateView->magic != BYPASS_STATE_MAGIC ||
        g_stateView->version != BYPASS_STATE_VERSION ||
        g_stateView->size != (unsigned int)sizeof(BypassState)) {
        UnmapViewOfFile(g_stateView);
        CloseHandle(g_stateMap);
        g_stateView = NULL;
        g_stateMap = NULL;
        return;
    }
    g_stateEvent = OpenEventA(SYNCHRONIZE, FALSE, BYPASS_STATE_EVT_NAME);
    g_stateFirst = 1;
}

static BOOL StateReadView(BypassState* out)
{
    int i;

    for (i = 0; i < 100; i++) {
        LONG s1 = g_stateView->seq;

        if ((s1 & 1) == 0) {
            MemoryBarrier();
            memcpy(out, (const void*)g_stateView, sizeof(*out));
            MemoryBarrier();
            if (g_stateView->seq == s1)
                return TRUE;
        }
        Sleep(0);
    }
    return FALSE;
}

static BOOL StateRefresh(void)
{
    int sw;

    if (!g_stateView) StateOpen();
    if (g_stateView) {
        if (!g_stateEvent)
            g_stateEvent = OpenEventA(SYNCHRONIZE, FALSE, BYPASS_STATE_EVT_NAME);
        if (!StateReadView(&g_stateSnap)) return FALSE;
        /* 主程序可能已经退出；配置文件仍然可人工修改，优先采用文件里的设置。 */
        if (g_configPath[0] &&
            GetFileAttributesA(g_configPath) != INVALID_FILE_ATTRIBUTES)
            StateReadConfig(&g_stateSnap, 0);
    } else {
        if (!StateReadConfig(&g_stateSnap, 1)) return FALSE;
    }


    g_stWindow = g_stateSnap.window_mode ? 1 : 0;
    g_stTitle = g_stateSnap.title_autohide ? 1 : 0;
    g_stMinTray = g_stateSnap.minimize_tray ? 1 : 0;
    g_stSmallW = g_stateSnap.small_w;
    g_stSmallH = g_stateSnap.small_h;
    g_stSmallPermille = g_stateSnap.small_permille;

    sw = GetSystemMetrics(SM_CXSCREEN);
    if (g_stSmallW > 0 && g_stSmallH > 0) {
        g_cfgW = g_stSmallW;
        g_cfgH = g_stSmallH;
    } else if (g_stSmallPermille > 0) {
        g_cfgW = sw * g_stSmallPermille / 1000;
        g_cfgH = 0;
    } else {
        g_cfgW = 0;
        g_cfgH = 0;
    }

    return TRUE;
}

static void StateWait(DWORD ms)
{
    if (g_stateEvent)
        WaitForSingleObject(g_stateEvent, ms);
    else
        Sleep(ms);
}

/* 在窗口"自己的线程"里完成的修复动作。
   ★ 为什么必须这样: SetWindowPos/SetWindowLong 施加到别的线程的窗口上时,
     调用线程会 SendMessage 过去等对方处理; 而对方的窗口线程如果正好在
     等我们的日志锁/子类化锁, 两边就互相卡死 (实测: 渲染线程卡在
     SetWindowPos 上, 整个画面停住)。所以 enforcer 只做"判断", 真正的
     修改用 PostMessage 丢回窗口线程自己执行。 */
#define WM_BYPASS_FIX   (WM_APP + 0x51)

static void FixOnOwnerThread(HWND h, int force)
{
    RECT     r;
    LONG_PTR st, ex;
    int      w, hh;

    if (GetPropA(h, PROP_PARKED)) return;   /* 已收起 -> 不做任何降级/样式修改 */
    /* ★ 已最小化/刚最小化到任务栏 -> 什么都不碰。
       以前这里只判 PARKED, 结果一条"排队中的修复消息"在用户点了最小化之后
       才执行, 会把 WS_MINIMIZE 清掉、顺手 SetWindowPos 摆回小窗 —— 表现就是
       "最小化后自己弹回来"。 */
    if (IsIconic(h) || TaskbarMinimized(h)) return;
    if (force == 2) {
        /* window-mode (control-side) small broadcast window: strip the
           WS_EX_TOPMOST it was created with; geometry stays untouched */
        ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) {
            real_SetWindowLongW(h, GWL_EXSTYLE, (LONG)(ex & ~WS_EX_TOPMOST));
            real_SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (InterlockedIncrement(&g_fixLogs) <= 40)
                HookLog("untopmost hwnd=%p (window-mode broadcast)", (void*)h);
        }
        return;
    }
    if (!GetWindowRect(h, &r)) return;
    w  = r.right - r.left;
    hh = r.bottom - r.top;
    st = GetWindowLongPtrW(h, GWL_STYLE);
    ex = GetWindowLongPtrW(h, GWL_EXSTYLE);

    if (force && IsWorthFixing(h)) {
        st &= ~(LONG_PTR)(WS_POPUP | WS_MAXIMIZE | WS_MINIMIZE);
        st |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
              WS_MAXIMIZEBOX;
        real_SetWindowLongW(h, GWL_STYLE, (LONG)st);
        real_SetWindowLongW(h, GWL_EXSTYLE, (LONG)(ex & ~WS_EX_TOPMOST));
        {
            int fx, fy, fw, fh;
            SmallRectFor(w, hh, &fx, &fy, &fw, &fh);
            real_SetWindowPos(h, HWND_NOTOPMOST, fx, fy, fw, fh,
                              SWP_FRAMECHANGED | SWP_NOACTIVATE);
            HookLog("FORCE small hwnd=%p %dx%d -> %dx%d",
                    (void*)h, w, hh, fw, fh);
        }
        MarkManaged(h);
        return;
    }

    if (LooksManaged(h)) {
        BOOL needStyle = ((ex & WS_EX_TOPMOST) || (st & WS_POPUP)) ? TRUE : FALSE;
        if (!needStyle) return;
        st &= ~(LONG_PTR)WS_POPUP;
        st |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX |
              WS_MAXIMIZEBOX;
        real_SetWindowLongW(h, GWL_STYLE, (LONG)st);
        real_SetWindowLongW(h, GWL_EXSTYLE, (LONG)(ex & ~WS_EX_TOPMOST));
        return;
    }

    /* 不是"值得修"的窗口 (老师本来就发的窗口模式、普通带框窗口) 一律不碰,
       即使宽度超过 2/3 屏 —— 否则显式/兜底降级会把 3/4 窗口又缩成 1/3。 */
    if (!IsWorthFixing(h)) return;

    if (!((ex & WS_EX_TOPMOST) || (st & WS_POPUP) || (st & WS_MAXIMIZE) ||
          w > g_maxW || hh > g_maxH))
        return;

    if (InterlockedIncrement(&g_fixLogs) <= 60) {
        char  cls[64] = "?";
        char  ttl[96] = "";
        DWORD opid = 0;
        char  img[MAX_PATH] = "?";
        GetClassNameA(h, cls, sizeof(cls));
        GetWindowTextA(h, ttl, sizeof(ttl) - 1);
        GetWindowThreadProcessId(h, &opid);
        if (opid != g_pid) GetPidImage(opid, img, sizeof(img));
        else               _snprintf(img, sizeof(img) - 1, "%s", g_ownImage);
        HookLog("fix hwnd=%p owner=%s cls=%s '%s' %dx%d st=0x%08X ex=0x%08X",
                (void*)h, img, cls, ttl, w, hh,
                (unsigned)st, (unsigned)ex);
    }

    st &= ~(LONG_PTR)(WS_POPUP | WS_MAXIMIZE | WS_MINIMIZE);
    st |= WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    real_SetWindowLongW(h, GWL_STYLE, (LONG)st);
    real_SetWindowLongW(h, GWL_EXSTYLE, (LONG)(ex & ~WS_EX_TOPMOST));

    {
        int nx, ny, nw, nh;
        SmallRectFor(w, hh, &nx, &ny, &nw, &nh);
        real_SetWindowPos(h, HWND_NOTOPMOST, nx, ny, nw, nh,
                          SWP_FRAMECHANGED | SWP_NOACTIVATE);
        {
            RECT nr;
            if (GetWindowRect(h, &nr)) {
                int aw = nr.right - nr.left, ah = nr.bottom - nr.top;
                if (InterlockedIncrement(&g_readLogs) <= 24)
                    HookLog("  -> now %dx%d (wanted %dx%d)", aw, ah, nw, nh);
                if ((aw != nw || ah != nh) && InterlockedIncrement(&g_mismatchLogs) <= 12) {
                    RECT cr;
                    GetClientRect(h, &cr);
                    HookLog("  !! readback mismatch hwnd=%p client=%dx%d "
                            "st=0x%08X ex=0x%08X",
                            (void*)h, cr.right - cr.left, cr.bottom - cr.top,
                            (unsigned)GetWindowLongPtrW(h, GWL_STYLE),
                            (unsigned)GetWindowLongPtrW(h, GWL_EXSTYLE));
                }
            }
        }
    }
    MarkManaged(h);

    /* 记下"用户矩形" = 刚降级出来的这个尺寸。
       之后最大化/还原都用它当 normal rect, 不会被系统的满屏矩形污染。 */
    {
        RECT nr;
        if (GetWindowRect(h, &nr)) SetUserRect(h, &nr);
    }
}

static BOOL CALLBACK EnforceProc(HWND h, LPARAM lp){
    RECT     r;
    LONG_PTR st, ex;
    int      w, hh;
    int*     fixedCount = (int*)lp;
    DWORD    opid = 0;

    if (g_disabled) return FALSE;
    if (!IsTargetWindow(h)) return TRUE;
    /* 用户主动最大化 (BypassUserMax / 已接管的系统最大化) -> 交给系统/用户自己管。
       ⚠ 未接管的系统最大化窗口不能在这里放过: 它很可能就是"注入晚了、应用
       已经把广播窗最大化了", 必须继续走降级 (DemoteWindowHard 会先 SW_RESTORE)。 */
    if (IsIconic(h)) {
        EnsureSubclassed(h);
        return TRUE;   /* 用户主动最小化 -> 不去"救"回来, 从任务栏恢复即可 */
    }
    if (IsZoomed(h) && (IsManaged(h) || GetPropA(h, PROP_USERMAX))) {
        EnsureSubclassed(h);
        return TRUE;
    }
    if (GetPropA(h, PROP_USERMAX)) {
        EnsureSubclassed(h);   /* 仍然要装钩子, 但不再降级 */
        return TRUE;
    }
    if (!IsWindowVisible(h)) return TRUE;
    if (IsIconic(h)) return TRUE;
    if (!GetWindowRect(h, &r)) return TRUE;

    /* 先确保这个窗口的过程被我们接管 —— 即使它现在已经是小窗也要装,
       否则渲染器下次想变回全屏时我们拦不住。 */
    EnsureSubclassed(h);

    w  = r.right - r.left;
    hh = r.bottom - r.top;

    TrackGeo(h, &r);
    GetWindowThreadProcessId(h, &opid);

    /* 自己进程的窗口: 一律"投递"给窗口线程, 绝不在 enforcer 线程直接改 */
    if (opid == g_pid) {
        if (!IsWorthFixing(h) && !LooksManaged(h)) {
            /* window-mode (control-side) small broadcast window: nothing ever
               demotes it, so the WS_EX_TOPMOST it was created with would stay
               forever.  Strip it on the window thread (force=2 = topmost only). */
            {
                char qb[64];
                qb[0] = 0;
                GetClassNameA(h, qb, sizeof(qb));
                if (IsRenderWindowClass(qb) &&
                    (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST))
                    PostMessageA(h, WM_BYPASS_FIX, 2, 0);
            }
            return TRUE;
        }

        st = GetWindowLongPtrW(h, GWL_STYLE);
        ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        if (LooksManaged(h)) {
            if ((ex & WS_EX_TOPMOST) || (st & WS_POPUP))
                PostMessageA(h, WM_BYPASS_FIX, 0, 0);
            return TRUE;
        }
        PostMessageA(h, WM_BYPASS_FIX, 0, 0);
        return TRUE;
    }

    /* 别的进程的窗口: 只有对方没有钩子时才由我们代劳 (配额 3 次) */
    if (PidHasDll(opid)) return TRUE;
    QueuePendingPid(opid);
    if (!CrossBudget(h)) {
        static HWND s_last;
        if (s_last != h) {
            char img[MAX_PATH] = "?";
            GetPidImage(opid, img, sizeof(img));
            HookLog("STUCK hwnd=%p owner=%s pid=%lu still %dx%d - owner hook missing?",
                    (void*)h, img, (unsigned long)opid, w, hh);
            s_last = h;
        }
        TryInjectOwner(opid);
        return TRUE;
    }

    FixOnOwnerThread(h, 0);
    (*fixedCount)++;
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 取证: 谁盖住了屏幕 (一次性 dump 所有 >= 半屏的可见顶层窗口)            */
/* ------------------------------------------------------------------ */

static int           g_dumpRound;

static BOOL CALLBACK DumpProc(HWND h, LPARAM lp)
{
    RECT r;
    int  w, hh, sw, sh;
    DWORD pid = 0;
    char cls[64] = "?";
    char img[MAX_PATH] = "?";
    LONG_PTR st, ex;

    (void)lp;
    if (!IsWindowVisible(h)) return TRUE;
    if (!GetWindowRect(h, &r)) return TRUE;

    sw = GetSystemMetrics(SM_CXSCREEN);
    sh = GetSystemMetrics(SM_CYSCREEN);
    w  = r.right - r.left;
    hh = r.bottom - r.top;
    if (w < sw / 2 || hh < sh / 2) return TRUE;      /* 只看大窗口 */

    GetWindowThreadProcessId(h, &pid);
    GetClassNameA(h, cls, sizeof(cls));
    GetPidImage(pid, img, sizeof(img));
    st = GetWindowLongPtrW(h, GWL_STYLE);
    ex = GetWindowLongPtrW(h, GWL_EXSTYLE);

    HookLog("big hwnd=%p pid=%lu %s cls=%s (%d,%d)-(%d,%d) %dx%d st=0x%08X ex=0x%08X%s%s",
            (void*)h, (unsigned long)pid, img, cls,
            (int)r.left, (int)r.top, (int)r.right, (int)r.bottom, w, hh,
            (unsigned)st, (unsigned)ex,
            (ex & 0x00080000) ? " LAYERED" : "",
            (ex & 0x00000020) ? " TRANSPARENT" : "");
    return TRUE;
}

static void DumpBigWindows(void)
{
    char desk[128];
    POINT pt;
    HWND  under;
    DWORD upid = 0;
    char  uimg[MAX_PATH] = "?";
    char  ucls[64] = "?";

    if (g_dumpRound >= 2) return;

    DesktopName(desk, sizeof(desk));

    pt.x = GetSystemMetrics(SM_CXSCREEN) / 2;
    pt.y = GetSystemMetrics(SM_CYSCREEN) / 2;
    under = WindowFromPoint(pt);
    if (under) {
        GetWindowThreadProcessId(under, &upid);
        GetClassNameA(under, ucls, sizeof(ucls));
        GetPidImage(upid, uimg, sizeof(uimg));
    }

    HookLog("---- big window dump #%d (desktop=%s) ----", g_dumpRound + 1, desk);
    HookLog("cursor-center window: hwnd=%p pid=%lu %s cls=%s",
            (void*)under, (unsigned long)upid, uimg, ucls);
    EnumWindows(DumpProc, 0);
    HookLog("---- end dump ----");
    g_dumpRound++;
}

/* 标题栏自动隐藏的"心跳": 不依赖窗口自己的 WM_TIMER。
   实测 (真实 ScreenRender.exe) 它的消息循环里 WM_TIMER 合成得很不规律,
   150ms 的 SetTimer 可能很久才被取到一次 —— 表现就是"最大化后标题栏
   一直不隐藏"。这里由 DLL 自己的线程每 250ms 给"用户最大化"的窗口补发
   一个 WM_BYPASS_CAP(2): 这是普通队列消息, 任何消息循环都会取到,
   不依赖 SetTimer/PeekMessage 的定时器合成规则。
   (仍然保留 WM_TIMER 那条路: 两条路都只是调 CaptionTick, 幂等。) */
static DWORD WINAPI CaptionPumpProc(LPVOID p)
{
    (void)p;
    for (;;) {
        int i;

        if (g_subCsReady) {
            EnterCriticalSection(&g_subCs);
            for (i = 0; i < MAX_SUBCLASS; i++) {
                HWND h = g_sub[i].h;
                if (!h || !IsWindow(h)) continue;
                if (GetPropA(h, PROP_USERMAX))
                    PostMessageA(h, WM_BYPASS_CAP, 2, 0);

                /* ★ 任务栏最小化锁的定时兜底: 窗口已经不在最小化了
                   (可能是系统/第三方进程直接改的状态, 不发 WINDOWPOSCHANGED),
                   而且锁已经打了 1.2 秒以上 —— 交给窗口线程决定是解锁还是压回去。
                   只 Post, 不在这里跨线程 ShowWindow (会死锁)。 */
                if (GetPropA(h, PROP_TASKBARMIN) && !IsIconic(h)) {
                    DWORD t = (DWORD)(ULONG_PTR)GetPropA(h, PROP_TASKBARMIN);
                    if (t && (DWORD)(GetTickCount() - t) > 1200)
                        PostMessageA(h, WM_BYPASS_REMIN, 0, 0);
                }
            }
            LeaveCriticalSection(&g_subCs);
        }
        Sleep(250);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* 内联 hook: 渲染器有一部分 ShowWindow 调用是用缓存的函数指针, 只改 IAT 拦不住 */
/* ------------------------------------------------------------------ */

/* user32 的导出经常是"跳板": FF 25 <绝对地址> (jmp dword ptr [IAT])。
   必须跟到真正实现再打补丁。 */
static void* FollowJump(void* target)
{
    unsigned char* b = (unsigned char*)target;
    int i;

    for (i = 0; i < 4; i++) {
        if (b[0] == 0xFF && b[1] == 0x25) {          /* jmp dword ptr [abs] */
            void** slot = (void**)(*(DWORD*)(b + 2));
            void*  t = slot ? *slot : NULL;
            if (!t || t == (void*)b) break;
            b = (unsigned char*)t;
            continue;
        }
        if (b[0] == 0xE9) {                          /* jmp rel32 */
            b = b + 5 + *(LONG*)(b + 1);
            continue;
        }
        if (b[0] == 0xEB) {                          /* jmp rel8 */
            b = b + 2 + (signed char)b[1];
            continue;
        }
        break;
    }
    return b;
}

/* 把 target 入口改成 jmp hook, 原 5 字节搬进 trampoline 再跳回 target+5。
   user32 的 ShowWindow(Async) 是 hotpatch 序言: 8B FF 55 8B EC, 正好 5 字节。
   ★ *realSlot 在写补丁**之前**就指向 trampoline, 避免打补丁的瞬间
     有线程调用 ShowWindow -> Hook -> real_ShowWindow 还在指向旧地址 -> 递归。 */
static BOOL InstallInlineHook(void* target, void* hook, void** realSlot)
{
    unsigned char* p;
    unsigned char* tr;
    DWORD          old;

    if (!target || !hook || !realSlot) return FALSE;
    p = (unsigned char*)FollowJump(target);
    if (p == (unsigned char*)hook) return FALSE;   /* 已经指向我们的 Hook */
    if (p != (unsigned char*)target)
        HookLog("inline hook: %p is a jump thunk -> real %p", target, p);
    if (!((p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 &&
           p[3] == 0x8B && p[4] == 0xEC) ||
          p[0] == 0xB8 ||    /* mov eax, imm32 —— win32u 的 syscall stub */
          p[0] == 0x68)) {   /* push imm32 —— 同为整 5 字节单指令 */
        /* 真正的实现不是"整 5 字节"的序言: 如果导出本身是 FF 25 跳板,
           就把跳板的操作数改成指向我们的指针槽 —— 不碰目标代码。 */
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
                        *realSlot = real;    /* 先更新, 再改跳板 */
                        *(DWORD*)(o + 2) = (DWORD)myslot;
                        VirtualProtect(o, 6, old2, &old2);
                        FlushInstructionCache(GetCurrentProcess(), o, 6);
                        HookLog("thunk hook installed: %p -> %p (real %p prologue %02X %02X %02X %02X %02X)",
                                target, hook, real, p[0], p[1], p[2], p[3], p[4]);
                        return TRUE;
                    }
                    VirtualFree(myslot, 0, MEM_RELEASE);
                }
            }
        }
        HookLog("inline hook: unexpected prologue at %p (%02X %02X %02X %02X %02X)",
                p, p[0], p[1], p[2], p[3], p[4]);
        return FALSE;
    }

    tr = (unsigned char*)VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    if (!tr) return FALSE;

    memcpy(tr, p, 5);
    tr[5] = 0xE9;
    *(LONG*)(tr + 6) = (LONG)((p + 5) - (tr + 5 + 5));   /* tr+10 -> target+5 */

    if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tr, 0, MEM_RELEASE);
        return FALSE;
    }
    *realSlot = tr;                     /* 先更新, 再打补丁 */
    p[0] = 0xE9;
    *(LONG*)(p + 1) = (LONG)((unsigned char*)hook - (p + 5));
    VirtualProtect(p, 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, 5);

    HookLog("inline hook installed: %p -> %p (trampoline %p)", p, hook, tr);
    return TRUE;
}

static DWORD WINAPI Worker(LPVOID p)
{
    HMODULE u32;
    int reps = 0;
    int patchTick = 0;
    (void)p;

    /* ⚠ 不能在 DllMain 里 LoadLibrary (加载器锁)。全部初始化搬到这个线程里:
       先确保 user32 已加载, 再解析真函数指针。 */
    u32 = GetModuleHandleA("user32.dll");
    if (!u32) u32 = LoadLibraryA("user32.dll");
    if (!u32) {
        HookLog("attach failed: user32.dll unavailable");
        return 0;
    }

    real_SetWindowPos       = (PFN_SetWindowPos)(void*)GetProcAddress(u32, "SetWindowPos");
    real_SetWindowLongW     = (PFN_SetWindowLongW)(void*)GetProcAddress(u32, "SetWindowLongW");
    real_SetWindowLongA     = (PFN_SetWindowLongA)(void*)GetProcAddress(u32, "SetWindowLongA");
    real_SetWindowLongPtrW  = (PFN_SetWindowLongPtrW)(void*)GetProcAddress(u32, "SetWindowLongPtrW");
    real_SetWindowLongPtrA  = (PFN_SetWindowLongPtrA)(void*)GetProcAddress(u32, "SetWindowLongPtrA");
    real_MoveWindow         = (PFN_MoveWindow)(void*)GetProcAddress(u32, "MoveWindow");
    real_SetWindowPlacement = (PFN_SetWindowPlacement)(void*)GetProcAddress(u32, "SetWindowPlacement");
    real_ShowWindow         = (PFN_ShowWindow)(void*)GetProcAddress(u32, "ShowWindow");
    real_ShowWindowAsync    = (PFN_ShowWindowAsync)(void*)GetProcAddress(u32, "ShowWindowAsync");
    real_ShowCursor         = (PFN_ShowCursor)(void*)GetProcAddress(u32, "ShowCursor");
    real_SetCursor          = (PFN_SetCursor)(void*)GetProcAddress(u32, "SetCursor");
    real_SwitchDesktop      = (PFN_SwitchDesktop)(void*)GetProcAddress(u32, "SwitchDesktop");
    real_SetWindowsHookExA  = (PFN_SetWindowsHookExA)(void*)GetProcAddress(u32, "SetWindowsHookExA");
    real_SetWindowsHookExW  = (PFN_SetWindowsHookExW)(void*)GetProcAddress(u32, "SetWindowsHookExW");

    {   /* CreateProcess* 在 kernel32 里 */
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        if (!k32) k32 = LoadLibraryA("kernel32.dll");
        if (k32) {
            real_CreateProcessA = (PFN_CreateProcessA)(void*)GetProcAddress(k32, "CreateProcessA");
            real_CreateProcessW = (PFN_CreateProcessW)(void*)GetProcAddress(k32, "CreateProcessW");
        }
    }
    /* ⚠ 32 位的 user32 不导出 SetWindowLongPtrW/A (winuser.h 里那两个只是
       映射到 SetWindowLongW/A 的宏), 所以不能把它们当成"必须存在"。 */
    if (!real_SetWindowPos || !real_SetWindowLongW || !real_SetWindowLongA ||
        !real_MoveWindow || !real_SetWindowPlacement || !real_ShowWindow ||
        !real_ShowCursor) {
        HookLog("attach failed: missing user32 exports "
                "(pos=%p lw=%p la=%p mv=%p wp=%p sw=%p sc=%p)",
                (void*)real_SetWindowPos, (void*)real_SetWindowLongW,
                (void*)real_SetWindowLongA, (void*)real_MoveWindow,
                (void*)real_SetWindowPlacement, (void*)real_ShowWindow,
                (void*)real_ShowCursor);
        return 0;
    }
    if (!real_SetWindowLongPtrW) real_SetWindowLongPtrW = (PFN_SetWindowLongPtrW)(void*)real_SetWindowLongW;
    if (!real_SetWindowLongPtrA) real_SetWindowLongPtrA = (PFN_SetWindowLongPtrA)(void*)real_SetWindowLongA;

    StateOpen();
    StateRefresh();
    ComputeSmallRect();

    /* 等目标进程把窗口建出来 */
    Sleep(200);

    /* 先认自己: 教师端进程一律不接管 (必须在打补丁之前判断) */
    {
        char full[MAX_PATH] = "?";
        GetModuleFileNameA(NULL, full, sizeof(full));
        BaseName(full, g_ownImage, sizeof(g_ownImage));
    }
    if (IsTeacherSideImage(g_ownImage)) {
        HookLog("skip: %s is teacher-side (no hooks installed)", g_ownImage);
        return 0;
    }

    /* ★ 内联 hook user32!ShowWindow/ShowWindowAsync:
       渲染器有些调用用的是缓存的函数指针, 只改 IAT 拦不住 —— 这就是
       "最小化后又被 ShowWindow(SW_SHOW) 顶回来" 的根因。
       成功时把 real_* 指向 trampoline, 我们的 Hook 内部继续调用原函数。
       ⚠ 必须放在"教师端进程直接退出"之后, 否则会把教师端的 user32 也改掉。 */
    /* virtual desktop pin: own STA thread, never COM on the renderer thread */
    VD_Start();
    if (real_ShowWindow) {
        if (!InstallInlineHook((void*)real_ShowWindow, (void*)Hook_ShowWindow,
                               (void**)&real_ShowWindow))
            HookLog("WARNING: inline ShowWindow hook failed (IAT only)");
    }
    if (real_ShowWindowAsync) {
        if (!InstallInlineHook((void*)real_ShowWindowAsync, (void*)Hook_ShowWindowAsync,
                               (void**)&real_ShowWindowAsync))
            HookLog("WARNING: inline ShowWindowAsync hook failed (IAT only)");
    }

    PatchAllModules();

    /* 标题栏自动隐藏的兜底心跳 (见 CaptionPumpProc): 不依赖 WM_TIMER */
    if (CreateThread(NULL, 0, CaptionPumpProc, NULL, 0, NULL))
        HookLog("caption pump started (250ms, WM_BYPASS_CAP)");
    else
        HookLog("WARNING: caption pump thread failed (err=%lu)", GetLastError());
    {
        char desk[128];
        DesktopName(desk, sizeof(desk));
        HookLog("attached: %s - IAT patched (%ld entries), small window = %dx%d at (%d,%d) "
                "[cfg=%dx%d, ratio follows source]",
                g_ownImage, (long)g_patchCount, g_smallW, g_smallH, g_smallX, g_smallY,
                g_cfgW, g_cfgH);
        HookLog("desktop=%s, window_mode=%s",
                desk,
                (WindowModeOn()) ? "on" : "off");
        if (ConsumeForcedMark()) g_forcedWindowMode = 1;
        g_isBroadcastProc = DetectBroadcastProc();
        HookLog("broadcast_proc=%d (%s)", g_isBroadcastProc,
                g_isBroadcastProc
                    ? "identity match: env/cmdline -> window is the broadcast, no size test"
                    : "not the broadcast renderer -> size fallback only");
        HookLog("dll=%s", g_dllPath[0] ? g_dllPath : "?");
        HookLog("build=%s %s", __DATE__, __TIME__);
        InitOwnDesktop();
        HookLog("own desktop=%s", g_ownDesktop[0] ? g_ownDesktop : "?");
        HookLog("window_mode=%s (bypass_config.ini)", WindowModeOn() ? "on" : "off");
        HookLog("title_autohide=%s (bypass_config.ini)", CaptionAutoHideOn() ? "on" : "off");
    }

    for (;;) {
        BOOL off = (GetFileAttributesA(g_offPath) != INVALID_FILE_ATTRIBUTES);
        int  fixedCount = 0;

        InterlockedExchange(&g_disabled, off ? 1 : 0);

        /* 配置/命令：共享内存 + 事件；没有映射时退回读 bypass_config.ini */
        StateRefresh();
        ComputeSmallRect();

        /* Two dump rounds during a broadcast (4s / 19s): enough to see which
           window covers the screen, without burying the log.  Only the
           broadcast renderer does it - every broadcast spawns a new process,
           and the student shell / MultiClient windows are never the culprit. */
        if (!off && g_isBroadcastProc && (reps == 8 || reps == 38))
            DumpBigWindows();

        if (!off) {
            /* 桌面守护: 万一被切到 NetTeachingDesktop, 把用户带回来 */
            EnsureWindowDesktop();

            EnumWindows(EnforceProc, (LPARAM)&fixedCount);

            /* 枚举结束后再做注入 —— 绝不能在上面的回调里等远程线程 */
            DrainPendingPids();

            /* 顺带把还没钩子的同族进程注上 (每 2 秒最多一轮) */
            SpreadSelf();

            /* 用户按了"小窗化"按钮 (共享内存 seq 变化) -> 强制降级一次。
               标志要在枚举前清掉, 否则本轮枚举 + 下轮轮询会各 FORCE 一次,
               导致多闪一下。 */

            if (fixedCount > 0) {
                HookLog("enforcer fixed %d window(s)", fixedCount);
            }
        }

        /* IAT 补丁: 前 30 秒每 500ms 扫一遍 (应对延迟加载的模块);
           之后降到每 5 秒扫一次 —— 旧版在这里彻底停了, 30 秒后才加载
           的模块永远不会被 hook。 */
        if (reps < 60) {
            PatchAllModules();
            reps++;
            StateWait(500);
        } else {
            if (++patchTick >= 5) { patchTick = 0; PatchAllModules(); }
            StateWait(fixedCount > 0 ? 300 : 1000);
        }
    }
    return 0;
}

/* ---- 桌面守护: 保证"用户正在看的桌面"就是"我们窗口所在的桌面" ----
   背景 (反编译实证): MultiClient.exe 会 CreateDesktop/SwitchDesktop 到
   "NetTeachingDesktop"; 一旦切过去, 学生自己的桌面 (explorer/任务栏/其它窗口)
   就不在屏幕上, 看起来就是"整屏黑掉、只剩右上角画面"。
   DLL 已经拦了启动期的 SwitchDesktop, 但有两种漏网:
     a) 切换发生在注入之前 (渲染器已经在教学桌面上);
     b) 别的进程 (非注入目标) 调用了 SwitchDesktop。
   这里做的是兜底: 每 2 秒比对一次"输入桌面"和"本进程所在桌面",
   不一致就切回本进程所在桌面 —— 注意是切回"窗口所在的桌面", 不是硬编码 Default,
   否则窗口本来就在教学桌面上时会被切走、反而看不见画面。 */
static DWORD g_lastDesktopCheck, g_lastDesktopLog;

static void DesktopNameOf(HDESK d, char* out, int n)
{
    out[0] = '\0';
    if (!d) return;
    if (!GetUserObjectInformationA(d, 2 /*UOI_NAME*/, out, n, NULL)) out[0] = '\0';
    out[n - 1] = '\0';
}

static void EnsureWindowDesktop(void)
{
    DWORD now = GetTickCount();
    HDESK hIn;
    char  inName[128];
    char  msg[300];

    if (now - g_lastDesktopCheck < 2000) return;
    g_lastDesktopCheck = now;
    if (!WindowModeOn() || g_disabled) return;
    if (!g_ownDesktop[0]) return;

    hIn = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!hIn) return;
    DesktopNameOf(hIn, inName, sizeof(inName));
    CloseDesktop(hIn);

    if (!inName[0]) return;
    if (_stricmp(inName, g_ownDesktop) == 0) return;

    /* ★ 只检测, 不切换 (2026-10-22 改为单一守护):
       主程序 bypass.exe 里有一个 1 秒看门狗, 它会切回 Default。
       如果这里也切回"窗口所在桌面", 两个守护会每秒互相对拉 -> 闪屏。
       统一策略: 以主程序为准 (保证用户能操作), DLL 只记录现场。 */
    if (now - g_lastDesktopLog > 30000) {
        g_lastDesktopLog = now;
        _snprintf(msg, sizeof(msg) - 1,
                  "desktop mismatch: input='%s' window='%s' "
                  "(main tool's DesktopGuard will restore Default)",
                  inName, g_ownDesktop);
        msg[sizeof(msg) - 1] = '\0';
        HookLog("%s", msg);
    }
}

static void InitOwnDesktop(void)
{
    HDESK d = GetThreadDesktop(GetCurrentThreadId());
    DesktopNameOf(d, g_ownDesktop, sizeof(g_ownDesktop));
}

/* ------------------------------------------------------------------ */
/* DllMain                                                             */
/* ------------------------------------------------------------------ */

static void BuildPaths(void)
{
    char dll[MAX_PATH];
    char* p;

    g_offPath[0] = '\0';
    g_logPath[0] = '\0';
    g_configPath[0] = '\0';

    if (!GetModuleFileNameA(g_self, dll, sizeof(dll))) return;

    _snprintf(g_dllPath, sizeof(g_dllPath) - 1, "%s", dll);
    g_dllPath[sizeof(g_dllPath) - 1] = '\0';

    p = strrchr(dll, '\\');
    if (!p) return;
    *(p + 1) = '\0';

    _snprintf(g_offPath, sizeof(g_offPath) - 1, "%sbypass_hook.off", dll);
    _snprintf(g_logPath, sizeof(g_logPath) - 1, "%sbypass_hook.log", dll);
    _snprintf(g_configPath, sizeof(g_configPath) - 1, "%s%s", dll, BYPASS_CONFIG_FILE);
    g_offPath[sizeof(g_offPath) - 1] = '\0';
    g_logPath[sizeof(g_logPath) - 1] = '\0';
    g_configPath[sizeof(g_configPath) - 1] = '\0';
}

/* 小窗统一用这套样式: 有标题栏/边框, 能拖动, 但去掉了 POPUP/置顶 */
#define SMALL_STYLE (WS_POPUP | WS_CAPTION | WS_THICKFRAME | \
                     WS_SYSMENU | WS_MINIMIZEBOX)

/* 按源窗口的宽高比算出小窗矩形。
   srcW/srcH: 源窗口(或它请求的)尺寸; 输出右下角贴边的小窗外框矩形。
   为什么要按比例: 渲染器是把画面铺满客户区的, 源窗口如果是近正方形,
   我们硬塞进 16:9 的小窗就会把视频拉扁/拉长。 */
static void SmallRectFor(int srcW, int srcH, int* px, int* py, int* pw, int* ph)
{
    RECT wa;
    int  sw = GetSystemMetrics(SM_CXSCREEN);
    int  sh = GetSystemMetrics(SM_CYSCREEN);
    int  w, h;

    if (srcW < 16) srcW = 16;
    if (srcH < 16) srcH = 16;

    if (g_cfgW > 0 && g_cfgH > 0) {
        /* 配置里写死了尺寸, 不推导比例 */
        w = g_cfgW;
        h = g_cfgH;
    } else {
        int cw = (g_cfgW > 0) ? g_cfgW : sw / 3;
        int ch;
        RECT rc;

        if (cw < 120) cw = 120;
        ch = (int)(srcH * cw / srcW);

        /* 太高就按半屏高反推宽度, 保证小窗不会占掉整个屏幕 */
        if (ch > sh / 2) {
            ch = sh / 2;
            cw = (int)(srcW * ch / srcH);
        }
        if (cw < 120) {
            cw = 120;
            ch = (int)(srcH * cw / srcW);
        }

        /* 视频是画在客户区里的, 所以先算出"客户区", 再加上标题栏/边框 */
        rc.left = rc.top = 0;
        rc.right = cw;
        rc.bottom = ch;
        if (!AdjustWindowRectEx(&rc, SMALL_STYLE, FALSE, 0)) {
            rc.left = rc.top = 0;
            rc.right = cw;
            rc.bottom = ch;
        }
        w = rc.right - rc.left;
        h = rc.bottom - rc.top;
    }

    wa.left = 0; wa.top = 0; wa.right = sw; wa.bottom = sh;
    SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);

    *px = wa.right - w - 24;
    *py = wa.bottom - h - 24;
    if (*px < 0) *px = 0;
    if (*py < 0) *py = 0;
    *pw = w;
    *ph = h;
}

static void ComputeSmallRect(void)
{
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    g_maxW = sw * 2 / 3;
    g_maxH = sh * 2 / 3;

    /* 只用于日志/默认值; 真正下发给窗口时都是按源窗口比例现算 */
    g_smallW = sw / 3;
    if (g_smallW < 320) g_smallW = 320;
    g_smallH = g_smallW * 9 / 16;
    SmallRectFor(160, 90, &g_smallX, &g_smallY, &g_smallW, &g_smallH);
}

/* 崩溃取证: 记录异常码/地址, 然后照常交给系统 (绝不吞掉异常)。
   有它就能判断崩的是不是我们的 DLL、崩在哪个地址。
   ★ 必须继续调用宿主原来的 UnhandledExceptionFilter: 直接覆盖会让应用自己的
     崩溃上报/清理逻辑失效 (SDL/渲染器可能自己设过)。 */
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevUef;

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (ep && ep->ExceptionRecord) {
        DWORD_PTR base = (DWORD_PTR)g_self;
        DWORD_PTR addr = (DWORD_PTR)ep->ExceptionRecord->ExceptionAddress;
        HookLog("!! crash code=0x%08X addr=%p%s",
                (unsigned)ep->ExceptionRecord->ExceptionCode, (void*)addr,
                (addr >= base && addr < base + 0x400000) ? " [inside our dll]" : "");
    }
    if (g_prevUef && g_prevUef != CrashFilter)
        return g_prevUef(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID reserved)
{
    (void)reserved;

    if (reason == DLL_PROCESS_ATTACH) {
        g_self = hInst;
        g_pid  = GetCurrentProcessId();
        DisableThreadLibraryCalls(hInst);

        InitializeCriticalSection(&g_logCs);
        g_logCsReady = 1;

        InitializeCriticalSection(&g_subCs);
        g_subCsReady = 1;

        BuildPaths();

        /* 崩溃取证 (只记录, 继续调用宿主原来的过滤器) */
        g_prevUef = SetUnhandledExceptionFilter(CrashFilter);

        /* DllMain 里只做这些"绝对安全"的事, 其余全交给 Worker 线程。
           这里绝不能 LoadLibrary / GetProcAddress / 等待任何东西。 */
        CreateThread(NULL, 0, Worker, NULL, 0, NULL);
    }
    /* DLL_PROCESS_DETACH 时不要 DeleteCriticalSection: Worker 线程可能还在跑,
       进程退出时由系统回收即可。 */
    return TRUE;
}
