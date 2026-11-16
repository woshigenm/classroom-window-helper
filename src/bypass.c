/*
 * Classroom Control Bypass Tool v1.2
 * 噢易多媒体网络教室 (V10.8.0.3895) 课堂控制解除工具
 *
 * ===========================================================================
 *  按钮功能对照表
 *  (鼠标停在任意按钮上时, 界面右下角会显示同一句话, 无需查文档)
 * ===========================================================================
 *
 *  「解锁键盘」        向键盘类设备发送实证控制码 0xB2008, 解除 KbFilter 的
 *                      按键拦截。首选 x64 原生 IOCTL; 失败才退回厂商
 *                      LockKeyboard.dll / 32 位 rundll32 代理。
 *                      （只有手动入口：点「解锁键盘」按钮；自动解锁已移除。）
 *
 *  「结束课堂进程」    按"先看守、后主体"的顺序终止课堂客户端, 否则主体会被
 *                      看守进程重新拉起:
 *                        ProcessProtect / Loader / WindowClientAuto
 *                        -> BlackSlient -> Student
 *                        -> MultiClient / MultiRender / ScreenRender / ...
 *
 *  「卸载过滤驱动」    停止并删除 KbFilter / ProcFireWall / FbdATS / easyusbflt /
 *                      OeNetLimit 服务, 并从键盘类 UpperFilters 里移除 KbFilter。
 *                      (只删服务不删 UpperFilters, 重启后系统仍会加载该驱动。)
 *
 *  「持续保持小窗」    主开关。开 = 注入 DLL 并持续压制广播窗; 关 = 写
 *                      bypass_hook.off 放行。小窗化本身由 DLL + 后台监视完成。
 *
 *  --silent            无人值守模式: 依次执行 提权 -> 清自启动 -> 结束进程 ->
 *                      卸载驱动 -> 小窗化 -> 复查 (内部走 AutoAllCore)。
 *
 *  「小窗化」          主开关是「持续保持小窗」。开 = 注入 bypass_hook32.dll 并持续压制满屏窗口；
 *                      关 = 写 bypass_hook.off，已注入的 DLL 2 秒内全部放行（不必重启渲染进程）。
 *                      机制:
 *                      1) 根治: 把 DLL 注入 MultiRender / ScreenRender / MultiClient 等渲染进程,
 *                         用 IAT hook 改掉它们自己的 SetWindowPos / SetWindowLong /
 *                         MoveWindow / ShowWindow:
 *                           - HWND_TOPMOST 一律改成 HWND_NOTOPMOST
 *                           - 尺寸超过屏幕 2/3 的一律改成右下角小窗
 *                           - 样式里的 WS_POPUP / WS_MAXIMIZE 一律抹掉
 *                         窗口从创建那一刻就是小窗, 所以不会出现"抢一次 -> 它改回去"的黑闪。
 *                      2) 补刀: 对注入之前就已经存在的窗口做一次降级(去置顶/加边框/缩到 1/3),
 *                         并回读验证; 之后由 WinEvent 钩子 + 自适应轮询兜底。
 *                      3) 预设"禁用全屏优化", 让独占全屏的渲染进程下次启动后能被降级。
 *
 *  「检查当前状态」    只读诊断: 管理员权限 / 驱动服务 / 关键进程状态 /
 *                      core.conf 的三个控制键 (含 View 实时值) / 当前输入桌面 /
 *                      解锁码验证情况。
 *
 * ===========================================================================
 *  代码分区 (按文件出现顺序)
 * ===========================================================================
 *   1.  常量定义            控件ID / 尺寸 / 配色
 *   2.  实证常量            IOCTL 0xB2004/0xB2008、设备路径、MCLOCK Desktop
 *   3.  目标列表            FILTER_DRIVERS / TARGET_PROCESSES / 自启动名单
 *   4.  UI 全局与按钮定义   BTNDEF 表 = 标签 + 一句话说明 + 配色
 *   5.  工具函数            提权、线程安全日志、进程枚举、路径探测
 *   6.  键盘解锁            KbFilter 控制码 + LockKeyboard.dll 兜底
 *   7.  桌面守护            MCLOCK Desktop / SwitchDesktop
 *   8.  进程终止            按看守->主体顺序
 *   9.  驱动卸载            服务 + 键盘类 UpperFilters 清理
 *   10. 自启动清理          Run/RunOnce 按"值内容"匹配
 *   11. 状态检查            只读诊断
 *   12. 广播窗口小窗化      Tier1.5 禁用全屏优化 + Tier1 分级降级 + 回读验证
 *   13. 后台线程
 *   14. UI 绘制与消息处理
 *
 * 命令行:
 *   bypass.exe                GUI 模式
 *   bypass.exe /auto          GUI 模式, 启动后自动跑一次自动流程
 *   bypass.exe /silent        静默模式 (无界面)
 *   bypass.exe /silent /auto  静默模式完整执行
 *
 * 编译 (RedPanda C++ MinGW64 gcc 11.5) —— 注意必须先修好 PATH:
 *   set PATH=C:\Program Files\RedPanda-Cpp\mingw64\bin;%PATH%
 *   gcc bypass.c -o bypass.exe -mwindows -D__USE_MINGW_ANSI_STDIO=1 ^
 *       -lgdi32 -lcomctl32 -ladvapi32 -lshell32 -luser32 -lshlwapi -lwinmm
 *   (mingw64\bin 不在 PATH 时, gcc 的子进程 cc1.exe 找不到 libwinpthread-1.dll,
 *    会以 0xC000007B 静默退出且不打印任何错误 —— 这就是"编译失败但无输出"的原因)
 *
 * 编译 (MSVC):
 *   cl bypass.c user32.lib gdi32.lib advapi32.lib shell32.lib shlwapi.lib comctl32.lib /link /SUBSYSTEM:WINDOWS
 *
 * 注意: 需要以管理员权限运行; Unicode 宽字符界面 + Per-Monitor V2 DPI 感知
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0603
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <shlwapi.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stddef.h>

#include "bypass_state.h"
#include "elevate.h"

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif
#ifndef DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
#define DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 ((HANDLE)-4)
#endif
#ifndef DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
#define DPI_AWARENESS_CONTEXT_SYSTEM_AWARE         ((HANDLE)-2)
#endif
/* NOTIFYICON_VERSION_4 的托盘回调值 (老 SDK 里没有) */
#ifndef NIN_SELECT
#define NIN_SELECT          (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT       (WM_USER + 1)
#endif
/*
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
*/


/* ========== 常量定义 ========== */
#define WINDOW_CLASS    L"ClassroomBypassClass"
#define WINDOW_TITLE    L"教学控制解除工具 v1.2"

/* 控件ID */
#define ID_BTN_UNLOCK_KB    1001
#define ID_BTN_KILL_PROC    1003
#define ID_BTN_UNINSTALL    1004
#define ID_BTN_CHECK_STATUS 1007
#define ID_BTN_EXIT         1009
#define ID_BTN_SMALLSTICKY  1015
#define ID_BTN_SCREENGUARD  1016    /* 屏幕监控守卫: 老师端只看到定格画面 */
#define ID_CHK_TRAY         1020    /* 广播窗最小化: 任务栏(真最小化) / 收起(托盘) */
#define ID_CHK_SHUT         1021    /* 屏蔽控制端重启/关机 (横幅/关机守卫) */
#define ID_LOG_OUTPUT       1100
#define ID_STATUS_BAR       1200
#define ID_lblStatus        1201
#define ID_HOTKEY_ESCAPE    1300    /* 全局热键 Ctrl+Alt+X: 鼠标被冻结时的逃生口 */
#define ID_HOTKEY_STICKY    1304    /* Ctrl+Alt+T 持续保持小窗 (开关) */
#define ID_HOTKEY_QUIT      1306    /* Ctrl+Alt+Q 退出 */
#define ID_HOTKEY_PANEL     1307    /* Ctrl+Alt+H 显示/隐藏主面板 */
#define ID_HOTKEY_DESKTOP   1308    /* Ctrl+Alt+D 立即切回 Default 桌面 */
#define ID_HOTKEY_CLICK     1309    /* Ctrl+Alt+E 应急点击 (穿过 MultiClient 的鼠标钩子) */
#define ID_HOTKEY_SCREEN    1310    /* Ctrl+Alt+M 屏蔽屏幕监控 (备用入口) */
#define ID_HOTKEY_LAST      ID_HOTKEY_SCREEN

/* 托盘 */
#define WM_TRAYICON         (WM_APP + 2)
#define TRAY_ICON_ID        1
#define TRAY_RETRY_MAX      20      /* NIM_ADD 失败后最多重试次数 */
#define TRAY_RETRY_MS       500     /* 重试间隔 */
#define IDT_TRAY_RETRY      0x52    /* 托盘图标重试定时器 (0x51 被 demote recheck 占用) */
#define IDT_BUILD_CHECK     0x53    /* 启动后 4 秒: 检查进程内的 DLL 是否与磁盘一致 */
#define IDM_TRAY_SHOW       2001
#define IDM_TRAY_UNLOCK     2004
#define IDM_TRAY_ABOUT      2005
#define IDM_TRAY_EXIT       2006
#define IDM_TRAY_RELEASE    2007    /* 恢复全屏广播 (写 bypass_hook.off) */
#define IDM_TRAY_DESKTOP    2008    /* 切回 Default 桌面 */
#define IDM_TRAY_CLICK      2009    /* 应急点击 */
#define IDM_TRAY_NETDIAG    2010    /* 网络/桌面诊断 (写日志) */
#define IDM_TRAY_SHUT       2011    /* 屏蔽控制端重启/关机 (开关) */

/* ========== UI theme constants (dark theme) ========== */
#define TITLEBAR_H      40      /* custom title bar height */
#define UI_PAD          16      /* content padding */
#define BTN_W           162     /* standard button width */
#define BTN2_W          (2 * BTN_W + BTN_GAP)  /* wide button: spans two columns */
#define BTN_H           42      /* standard button height */
#define BTN_GAP         10      /* gap between buttons */
#define BTN_RADIUS      8       /* rounded corner radius */
#define BTN_MAX         32      /* max tracked buttons */

#define ROW1_Y          78      /* button row 1 y */
#define ROW2_Y          130     /* button row 2 y */
#define ROW3_Y          182     /* button row 3 y (screen guard, full width) */
#define CHK_Y           236     /* "broadcast minimize" checkbox y */
#define LOGLBL_Y        264     /* log label y */
#define LOG_Y           288     /* log area y */

#define UI_BG           RGB(24, 26, 32)      /* window background */
#define UI_TITLE_BG     RGB(18, 20, 26)      /* title bar background */
#define UI_BORDER       RGB(58, 62, 72)      /* border / separator */
#define UI_INPUT_BG     RGB(38, 41, 50)      /* edit box background */
#define UI_LOG_BG       RGB(16, 18, 23)      /* log area background */
#define UI_TEXT         RGB(238, 240, 243)   /* main text */
#define UI_TEXT_DIM     RGB(148, 154, 165)   /* secondary text */
#define UI_ACCENT       RGB(0, 120, 212)     /* primary blue */
#define UI_ACCENT_HV    RGB(29, 138, 229)    /* blue hover */
#define UI_ACCENT_DN    RGB(0, 90, 160)      /* blue pressed */
#define UI_SUCCESS      RGB(16, 137, 62)     /* green */
#define UI_SUCCESS_HV   RGB(28, 160, 80)
#define UI_SUCCESS_DN   RGB(10, 108, 48)
#define UI_DANGER       RGB(196, 43, 28)     /* red */
#define UI_DANGER_HV    RGB(220, 68, 52)
#define UI_DANGER_DN    RGB(158, 32, 20)
#define UI_GRAY_BTN     RGB(58, 62, 72)      /* neutral button */
#define UI_GRAY_HV      RGB(72, 78, 90)
#define UI_GRAY_DN      RGB(44, 48, 56)
#define UI_CLOSE_HV     RGB(232, 17, 35)     /* title close hover (win red) */
#define UI_MIN_HV       RGB(64, 68, 78)      /* title min hover */

/* ------------------------------------------------------------------
 * 键盘过滤驱动控制码 —— REA/Ghidra 实证 (不再是猜测)
 *
 * KbFilter.sys 的 IRP_MJ_DEVICE_CONTROL 分发函数 (VA 0x110ac) 中:
 *
 *   if (IoControlCode == 0xB2004)  状态 = 2  "KEYBOARD_ALL_DISALE"
 *   if (IoControlCode == 0xB2008)  状态 = 1  "KEYBOARD_ALL_RELEASE"        <- 解锁
 *   if (IoControlCode == 0xB200C)  状态 = 2  "KEYBOARD_ALL_DISALE_EXCEPT_CTRLENTER"
 *   if (IoControlCode == 0xB2010)  状态 = 3  "KEYBOARD_ALL_DISALE_EXCEPT_PASSWORD"
 *   if (IoControlCode == 0xB2014)  状态 = 4  "KEYBOARD_ALL_DISALE_EXCEPT_PASSWORD_AND_CHINESE"
 *   其它控制码 -> 直接透传给下层驱动
 *
 * 控制码布局 = CTL_CODE(FILE_DEVICE_KEYBOARD(0x0B), 0x801+n, METHOD_BUFFERED, FILE_ANY_ACCESS)
 * 即 0xB2004 = 0x0B<<16 | 0x2<<14 | 0x801<<2 | 0 = 全锁, 0xB2008 = 解锁。
 * 驱动内部只保存一个状态字节, 没有任何口令/进程校验, 因此发送 0xB2008 即可解锁。
 *
 * LockKeyboard.dll!EnableKeyboard (VA 0x10001420) 的实际动作:
 *   DefineDosDeviceW(DDD_RAW_TARGET_PATH, L"Kbd", L"\\Device\\KeyboardClass0")
 *   CreateFileW(L"\\\\.\\Kbd", GENERIC_WRITE, FILE_SHARE_READ|WRITE, NULL, OPEN_EXISTING, 0, NULL)
 *   DeviceIoControl(h, 0xB2008, NULL, 0, NULL, 0, &ret, NULL)
 * 然后对 KeyboardClass1 重复一次。
 * ------------------------------------------------------------------ */
#define IOCTL_KB_FILTER_UNLOCK             0xB2008   /* 解锁 (KEYBOARD_ALL_RELEASE) */
#define IOCTL_KEYBOARD_ALL_DISABLE         0xB2004   /* 全锁 */
#define IOCTL_KEYBOARD_ALL_RELEASE         0xB2008   /* 解锁 */
#define IOCTL_KEYBOARD_ALL_DISABLE_EX_CTR  0xB200C   /* 锁(除 Ctrl+Enter) */
#define IOCTL_KEYBOARD_ALL_DISABLE_EX_PWD  0xB2010   /* 锁(仅允许口令输入) */
#define IOCTL_KEYBOARD_ALL_DISABLE_EX_PWD_CHS 0xB2014 /* 锁(口令+中文) */
#define IOCTL_VENDOR_MAIN                  0x02200000 /* 仅作参考, 非键盘锁码 */

/* 设备路径 */
/* KbFilter 驱动自身控制设备: \DosDevices\OeKbdFilter -> \Device\DevOeKbdFilter */
#define DEVICE_KBD_LINK     "\\\\.\\OeKbdFilter"
#define DEVICE_KBD_KERNEL   "\\Device\\DevOeKbdFilter"
/* 键盘类设备 (锁码真正被处理的地方, 厂商 DLL 也是操作它们) */
#define DEVICE_KEYBOARD_0   "\\Device\\KeyboardClass0"
#define DEVICE_KEYBOARD_1   "\\Device\\KeyboardClass1"
#define DEVICE_KBD_DOSNAME  L"Kbd"      /* 临时 DOS 设备名, 与厂商 DLL 一致 */

/* ------------------------------------------------------------------
 * 黑屏/桌面锁定 —— REA/Ghidra 实证 (systemoper.dll)
 *
 * 上锁 (FUN_10008010 分支2):
 *   CreateDesktopA("MCLOCK Desktop", NULL, NULL, 0, DESKTOP_ALL_ACCESS, NULL)
 *   SetThreadDesktop(hDesk); SwitchDesktop(hDesk);
 *   改 HKCU\Control Panel\Desktop 的 TileWallpaper/WallpaperStyle (备份后置 0/2)
 *   LoadLibraryW("LockKeyboard.dll") -> DisableKeyboard()   (即 IOCTL 0xB2004)
 *
 * 解锁 (FUN_10008010 分支1):
 *   读取备份的原始桌面名 -> SetThreadDesktop + SwitchDesktop(原桌面)
 *   恢复壁纸注册表值 -> EnableKeyboard()                    (即 IOCTL 0xB2008)
 *
 * 结论: 锁定桌面与默认桌面在同一个窗口站 (WinSta0), 所以本进程直接
 * SwitchDesktop(OpenDesktop("Default")) 是可行的, 无需被杀进程的配合。
 * ------------------------------------------------------------------ */
#define LOCK_DESKTOP_NAME   "MCLOCK Desktop"
#define DEFAULT_DESKTOP_NAME "Default"

/* 驱动服务名 */
#define DRIVER_SERVICE_NAME "KbFilter"

/* 所有需要卸载的过滤驱动 (REA 实证的服务名: DriverInstall.exe /d1=.. /d2=..) */
static const char* FILTER_DRIVERS[] = {
    "KbFilter",       /* 键盘过滤 (黑屏/锁键时真正拦截按键的驱动) */
    "ProcFireWall",   /* 进程保护: 会阻止 OpenProcess/TerminateProcess */
    "FbdATS",         /* 文件/磁盘过滤 */
    "easyusbflt",     /* USB 过滤 */
    "OeNetLimit",     /* 网络限速 */
    NULL
};

/* ------------------------------------------------------------------
 * 需要终止的进程; 顺序 = 先杀"守保/看守"再杀主体,
 * 否则 Loader.exe / WindowClientAuto.exe 会把 Student.exe 拉起来。
 * REA 实证: Loader.exe 里含 "Call ProcessProtect"/"ProcessProtect.exe",
 *           Student.exe 里含 "StudentRunning"/"ProcessProtect.exe"/"x86\DeviceControl_x86.exe"。
 * ------------------------------------------------------------------ */
static const char* TARGET_PROCESSES[] = {
    /* 1. 看守/守护类 (必须最先杀, 否则主体会被拉起) */
    "ProcessProtect.exe",
    "Loader.exe",
    "WindowClientAuto.exe",
    /* 2. 黑屏/锁屏 */
    "BlackSlient.exe",
    /* 3. 学生端主体 */
    "Student.exe",
    /* 4. 广播/投屏渲染链 */
    "MultiClient.exe",
    "MultiRender.exe",
    "ScreenRender.exe",
    "AudioRender.exe",
    "SharedDesktop.exe",
    "StudentScreenHelper.exe",
    /* 5. 教师端服务进程 (学生机上一般不存在, 存在则一并处理) */
    "ScreenSender.exe",
    "AudioSender.exe",
    NULL
};

/* 需要从 Run/RunOnce 自启动项里清除的进程名 (按值内容匹配, 不依赖具体键名) */
static const char* STARTUP_PROCESS_NAMES[] = {
    "Student.exe",
    "Loader.exe",
    "WindowClientAuto.exe",
    "Startup.exe",
    "client_console.exe",
    "transfer_console.exe",
    "DeviceControl_x86.exe",
    "DeviceControl_x64.exe",
    NULL
};

/* ========== 全局变量 ========== */
static HWND g_hwndLog = NULL;
static HWND g_hwndMain = NULL;

/* 静默模式 */
static BOOL g_silentMode = FALSE;
static BOOL g_autoMode = FALSE;
static FILE* g_logFile = NULL;

/* 主线程ID (用于判断调用方是否在 UI 线程) */
static DWORD g_uiThreadId = 0;

/* IOCTL扫描记录 */
static DWORD g_workingIoctl = 0;

/* 监控线程 */

/* 广播窗口小窗化 */
static volatile BOOL g_smallWinRun = FALSE;
static HANDLE g_hSmallWinThread = NULL;

/* ========== UI globals ========== */

/* ============================================================================
 *  BTNDEF —— 按钮的唯一定义源 (改这里即可, 无需再去别处找)
 *
 *    id   : 控件 ID, 同时也是 WM_COMMAND 的分支值 (见 ID_BTN_* 宏)
 *    text : 按钮默认标签。开关类按钮的标签在 DrawCustomButton 里会动态附加
 *           开/关状态, 这里写的是静态部分。
 *    desc : 一句话功能说明。鼠标停在按钮上时显示在界面右下角状态区
 *           (见 SetHoverHint / WM_PAINT), 是"这个按钮到底干什么"的权威来源。
 *    base/hv/dn : 正常 / 悬停 / 按下 三种底色
 *    bold : 是否用粗体字 (仅"退出")
 * ========================================================================== */
typedef struct {
    int            id;
    const wchar_t* text;
    const wchar_t* desc;
    COLORREF       base;
    COLORREF       hv;
    COLORREF       dn;
    int            bold;
    const wchar_t* hot;      /* 全局快捷键说明 (NULL = 这个按钮没有) */
} BTNDEF;

/* hover state per button */
typedef struct {
    BOOL hover;
} BTNSTATE;

/* 按钮顺序 = 界面上的排列顺序。每行 2 个, 与 CreateControls 的布局一致。 */
static const BTNDEF g_btnDefs[] = {
    /* ---- 第 1 行: 即时响应 ---- */
    { ID_BTN_UNLOCK_KB,   L"解锁键盘", L"发解锁码 0xB2008, 解除 KbFilter 的键盘拦截",
      UI_ACCENT,      UI_ACCENT_HV,  UI_ACCENT_DN,  0, NULL },
    { ID_BTN_KILL_PROC,   L"结束课堂进程", L"按住看守->主体顺序结束课堂各进程",
      UI_DANGER,      UI_DANGER_HV,  UI_DANGER_DN,  0, NULL },

    /* ---- 第 2 行: 持久化改动 + 投屏小窗化 ---- */
    { ID_BTN_UNINSTALL,   L"卸载过滤驱动", L"停删 KbFilter 等驱动服务并清键盘类 UpperFilters",
      RGB(150,30,60), RGB(180,50,80), RGB(118,20,46), 0, NULL },
    { ID_BTN_SMALLSTICKY, L"持续保持小窗", L"开关: 开=注入并持续压制; 关=写 bypass_hook.off 全部放行",
      UI_GRAY_BTN,    UI_GRAY_HV,    UI_GRAY_DN,    0, L"Ctrl+Alt+T" },

    /* ---- 第 3 行: 屏幕监控守卫 (整行) ---- */
    { ID_BTN_SCREENGUARD, L"屏蔽屏幕监控", L"开关: 开=老师端只能看到定格画面(不传实时桌面); 关=正常",
      UI_SUCCESS,     RGB(60,170,110), RGB(40,140,90), 0, L"Ctrl+Alt+M" },

    /* ---- 底部按钮 (不参与网格布局) ---- */
    { ID_BTN_CHECK_STATUS,L"检查当前状态", L"只读诊断: 权限/驱动/进程/配置/当前桌面",
      UI_GRAY_BTN,    UI_GRAY_HV,    UI_GRAY_DN,    0, NULL },
    { ID_BTN_EXIT,        L"退出",     L"退出程序 (不会解除当前锁定状态)",
      RGB(150,30,60), RGB(180,50,80), RGB(118,20,46), 0, L"Ctrl+Alt+Q" },
};

static BTNSTATE g_btnSt[BTN_MAX];
static HFONT  g_hFontBtn, g_hFontBtnBold, g_hFontTitle, g_hFontSection, g_hFontLog, g_hFontHint;
static HBRUSH g_hbrBg, g_hbrTitleBg, g_hbrBorder, g_hbrInputBg, g_hbrLogBg, g_hbrCloseHv, g_hbrMinHv;
static char   g_statusText[128] = "\xE5\xB0\xB1\xE7\xBB\xAA - \xE9\xBC\xA0\xE6\xA0\x87\xE5\x81\x9C\xE5\x9C\xA8\xE6\x8C\x89\xE9\x92\xAE\xE4\xB8\x8A\xE5\x8F\xAF\xE7\x9C\x8B\xE5\x8A\x9F\xE8\x83\xBD\xE8\xAF\xB4\xE6\x98\x8E";
static wchar_t g_hoverHint[200] = L"";   /* 悬停按钮时的一句话说明, 空 = 未悬停 */
static int     g_hoverHintId = 0;         /* 说明当前属于哪个按钮 (防止相邻按钮 leave 乱序误清) */
static RECT   g_rcStatusArea = { 0, 0, 0, 0 };
static RECT   g_rcTitleMin = { 0, 0, 0, 0 };
static RECT   g_rcTitleClose = { 0, 0, 0, 0 };
static int    g_hoverTitle = 0;      /* 0 = none, 1 = min, 2 = close */
static BOOL   g_titleTracking = FALSE;
/* 屏幕监控守卫 (bin\bypass_screen.ini, 由注入的 DLL 在 0.5s 内读到) */
static BOOL   g_screenGuard = FALSE;
/* 远程重启/关机守卫 (bin\bypass_shut.ini, 同样由注入的 DLL 读) */
static BOOL   g_shutGuard = FALSE;

/* DPI awareness (Per-Monitor V2, no manifest needed) */
static UINT g_dpi    = 96;
static int  g_titleH = 40;         /* scaled title bar height */

typedef UINT (WINAPI *PFN_GetDpiForWindow)(HWND);
static PFN_GetDpiForWindow g_pfnGetDpiForWindow = NULL;

/* logical px -> device px */
static int S(int v) {
    return MulDiv(v, (int)g_dpi, 96);
}

static void InitDpiApis(void) {
    HMODULE hU = GetModuleHandleW(L"user32.dll");
    if (hU) {
        g_pfnGetDpiForWindow = (PFN_GetDpiForWindow)(void*)
            GetProcAddress(hU, "GetDpiForWindow");
    }
}

static UINT GetWindowDpiSafe(HWND hWnd) {
    if (hWnd && IsWindow(hWnd) && g_pfnGetDpiForWindow) {
        UINT d = g_pfnGetDpiForWindow(hWnd);
        if (d >= 96) return d;
    }
    {
        HDC hdc = GetDC(NULL);
        UINT d = 96;
        if (hdc) {
            int y = GetDeviceCaps(hdc, LOGPIXELSY);
            if (y > 0) d = (UINT)y;
            ReleaseDC(NULL, hdc);
        }
        return d;
    }
}

/* ========== 函数前向声明 ========== */
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void CreateControls(HWND hwnd);
void AppendLog(HWND hwnd, const char* fmt, ...);
void UpdateStatus(HWND hwnd, const char* status);
void SafeLog(const char* fmt, ...);

BOOL UnlockKeyboard(HWND hwnd);
BOOL KillControlProcesses(HWND hwnd);
BOOL UninstallDriver(HWND hwnd);
BOOL CheckStatus(HWND hwnd);

BOOL KillProcessByName(const char* name);
BOOL IsProcessRunning(const char* name);
BOOL IsDriverInstalled(void);
BOOL IsUserAnAdmin(void);

static BOOL FindSoftwareFile(const char* relName, char* outPath, DWORD size);

/* broadcast small-window */
static int SmallWindowBroadcastCore(HWND hwndLog, BOOL quiet);
static int PresetFullscreenDowngrade(HWND hwndLog, BOOL quiet);
void StartSmallWin(HWND hwnd);
void StopSmallWin(HWND hwnd);
void OnSmallStickyToggle(HWND hwnd);

/* UI helpers */
static void CreateUIFonts(void);
static void DestroyUIFonts(void);
static void FillRoundedRect(HDC hdc, RECT rc, int radius, COLORREF color);
static void FrameRoundedRect(HDC hdc, RECT rc, int radius, COLORREF color);
static const BTNDEF* GetBtnDef(int id);
static BTNSTATE* GetBtnState(int id);
static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR uId, DWORD_PTR dw);
static void DrawCustomButton(const DRAWITEMSTRUCT* dis);
static void DrawTitleBar(HWND hwnd, HDC hdc);
static void LayoutControls(HWND hwnd);
static BOOL ReadMinimizeTaskbarMode(void);
static void WriteMinimizeMode(BOOL taskbar);
static void StateInit(void);
static void ShutGuardLoad(void);
static void ShutGuardSave(void);
static void OnShutGuardToggle(HWND hwnd);

/* 托盘 (notification area) */
static HICON g_trayIcon;
static BOOL  g_trayAdded;
static BOOL  g_trayBalloonShown;
static UINT  g_msgTaskbarCreated;   /* Explorer 重启广播 (RegisterWindowMessage) */
static int   g_trayRetries;         /* NIM_ADD 失败次数 */
static void TrayAdd(HWND hwnd);
static void TrayRemove(HWND hwnd);
static void TrayUpdateTip(HWND hwnd, const wchar_t* tip);
static void TrayShowMenu(HWND hwnd);
static void StartDesktopGuard(HWND hwnd);
static BOOL RestoreDefaultDesktop(char* fromOut, DWORD fromSize);
static void EmergencyClick(void);
static void LogNetStatus(HWND hwnd);
static void ShowMainWindow(HWND hwnd);
static void HideToTray(HWND hwnd);

/* ========== 工具函数 ========== */

/* 检查是否以管理员权限运行 */
BOOL IsUserAnAdmin(void) {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuth, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

/* 启用单个特权。以管理员身份运行时, SeDebugPrivilege 在令牌里默认是"禁用"状态,
   不显式启用会导致 OpenProcess(PROCESS_TERMINATE) 对其它用户/受保护进程失败。 */
BOOL EnablePrivilege(const char* name) {
    HANDLE hToken = NULL;
    TOKEN_PRIVILEGES tp;
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(),
            TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return FALSE;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueA(NULL, name, &tp.Privileges[0].Luid)) {
        SetLastError(ERROR_SUCCESS);
        if (AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL)) {
            ok = (GetLastError() == ERROR_SUCCESS);
        }
    }
    CloseHandle(hToken);
    return ok;
}

/* 一次性打开本工具需要的全部特权 */
void EnableRequiredPrivileges(void) {
    static const char* privs[] = {
        "SeDebugPrivilege",         /* 打开/终止其它会话或受保护进程 */
        "SeShutdownPrivilege",      /* 关机/重启 */
        "SeLoadDriverPrivilege",    /* 加载驱动 */
        "SeTakeOwnershipPrivilege", /* 接管注册表/文件所有权 */
        "SeRestorePrivilege",
        NULL
    };
    for (int i = 0; privs[i]; i++) EnablePrivilege(privs[i]);
}

/* 日志互斥。★ 以前是自旋锁 (InterlockedExchange + Sleep(0)) 包着 fputs+fflush:
   日志文件被杀软扫描 / 放在慢盘上时, 别的线程 (包括 UI 线程) 会在这段里
   100% 空转一个核。改成临界区 —— 阻塞的线程让出 CPU, 等待时间也不会更长。 */
static CRITICAL_SECTION g_logCs;
static int              g_logCsReady;

static void LogLock(void)   { if (g_logCsReady) EnterCriticalSection(&g_logCs); }
static void LogUnlock(void) { if (g_logCsReady) LeaveCriticalSection(&g_logCs); }

static void LogInit(void)
{
    if (g_logCsReady) return;
    InitializeCriticalSection(&g_logCs);
    g_logCsReady = 1;
}

/* 日志输出到编辑框 (同时支持静默模式) */
void AppendLog(HWND hwnd, const char* fmt, ...) {
    char buffer[1024];
    char fmtA[1024];          /* 必须在函数作用域: fmt 会指向它, 出了内层块就是野指针 */
    wchar_t wfmt[1024];
    va_list args;

    /* C21: 源码里的格式串是 UTF-8, 而 %s 填进来的运行时串 (进程路径等) 是
       ACP。混在一次 vsnprintf 里必然有一边乱码。这里把格式串先转成 ACP,
       让整条输出统一按 ACP 编码。 */
    if (MultiByteToWideChar(CP_UTF8, 0, fmt, -1, wfmt, 1024) > 0 &&
        WideCharToMultiByte(CP_ACP, 0, wfmt, -1, fmtA, sizeof(fmtA), NULL, NULL) > 0) {
        fmt = fmtA;
    }

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    
    /* 静默模式: 输出到控制台和日志文件 */
    LogLock();
    if (g_silentMode) {
        fputs(buffer, stdout);
        fflush(stdout);
    }
    if (g_logFile) {
        fputs(buffer, g_logFile);
        fflush(g_logFile);
    }
    LogUnlock();
    
    if (hwnd == NULL) return;

    /* 工作线程不得直接 SendMessage 到 UI 控件: 若 UI 线程正阻塞在
       StopSmallWin 的 WaitForSingleObject 上, 双方会死锁。
       非 UI 线程统一改走 PostMessage 队列 (由 WndProc 的 WM_APP+1 处理)。 */
    if (g_uiThreadId != 0 && GetCurrentThreadId() != g_uiThreadId) {
        char* copy = (char*)malloc(strlen(buffer) + 1);
        if (copy) {
            strcpy(copy, buffer);
            if (!PostMessage(g_hwndMain, WM_APP + 1, 0, (LPARAM)copy)) free(copy);
        }
        return;
    }

    HWND hLog = GetDlgItem(hwnd, ID_LOG_OUTPUT);
    if (!hLog) return;

    /* buffer 现在是 ACP, 转成 Unicode 给 edit 控件 (cp 必须一致) */
    wchar_t wbuf[2048];
    MultiByteToWideChar(CP_ACP, 0, buffer, -1, wbuf, 2048);
    wbuf[2047] = L'\0';

    int len = GetWindowTextLengthW(hLog);
    int wlen = lstrlenW(wbuf);
    SendMessageW(hLog, EM_SETSEL, len, len);
    SendMessageW(hLog, EM_REPLACESEL, FALSE, (LPARAM)wbuf);
    SendMessageW(hLog, EM_SETSEL, len + wlen, len + wlen);
    SendMessageW(hLog, EM_SCROLLCARET, 0, 0);
}

/* 线程安全日志 (使用全局主窗口句柄, PostMessage到UI线程) */
void SafeLog(const char* fmt, ...) {
    char buffer[1024];
    char fmtA[1024];
    wchar_t wfmt[1024];
    va_list args;

    /* 与 AppendLog 保持一致: 格式串 (UTF-8) 先转 ACP, 让整条日志只有一种编码,
       否则中文格式串 + ACP 路径混在一起必然乱码。 */
    if (MultiByteToWideChar(CP_UTF8, 0, fmt, -1, wfmt, 1024) > 0 &&
        WideCharToMultiByte(CP_ACP, 0, wfmt, -1, fmtA, sizeof(fmtA), NULL, NULL) > 0)
        fmt = fmtA;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    if (g_silentMode) {
        LogLock();
        fputs(buffer, stdout);
        fflush(stdout);
        LogUnlock();
    }
    if (g_logFile) {
        LogLock();
        fputs(buffer, g_logFile);
        fflush(g_logFile);
        LogUnlock();
    }

    if (g_hwndLog && g_hwndMain) {
        char* copy = (char*)malloc(strlen(buffer) + 1);
        if (copy) {
            strcpy(copy, buffer);
            if (!PostMessage(g_hwndMain, WM_APP + 1, 0, (LPARAM)copy)) {
                free(copy);
            }
        }
    }
}

/* 更新状态栏 */
void UpdateStatus(HWND hwnd, const char* status) {
    if (status) {
        strncpy(g_statusText, status, sizeof(g_statusText) - 1);
        g_statusText[sizeof(g_statusText) - 1] = '\0';

        /* 托盘提示同步显示状态: 窗口收起时也能知道当前在干什么 */
        {
            wchar_t wstat[128] = L"";
            if (MultiByteToWideChar(CP_ACP, 0, status, -1, wstat, 128) > 0) {
                wchar_t tip[160];
                _snwprintf(tip, 159, L"教学控制解除工具 — %s", wstat);
                tip[159] = 0;
                TrayUpdateTip(hwnd, tip);
            }
        }
    }
    if (hwnd) {
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

/* 清空日志 */
void ClearLog(HWND hwnd) {
    HWND hLog = GetDlgItem(hwnd, ID_LOG_OUTPUT);
    if (hLog) {
        SetWindowText(hLog, "");
    }
}

/* ========== 进程操作函数 ========== */

/* 根据名称终止进程 */
BOOL KillProcessByName(const char* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    DWORD  pids[32];
    int    npid = 0, i;
    BOOL   found = FALSE;
    BOOL   truncated = FALSE;

    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);

    /* 先在枚举循环里只收集 PID —— 不要在 Process32Next 循环里
       WaitForSingleObject(3000); 多实例时每个都会把界面卡 3 秒。 */
    if (Process32First(snapshot, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0 && npid < 32) {
                pids[npid++] = pe.th32ProcessID;
            } else if (_stricmp(pe.szExeFile, name) == 0 && npid >= 32) {
                truncated = TRUE;   /* 同名实例超过 32 个: 剩下的杀不到, 要留证据 */
            }
        } while (Process32Next(snapshot, &pe));
    }
    CloseHandle(snapshot);

    for (i = 0; i < npid; i++) {
        HANDLE hProcess = OpenProcess(
            PROCESS_TERMINATE | PROCESS_QUERY_INFORMATION | SYNCHRONIZE,
            FALSE, pids[i]);
        if (!hProcess) {
            /* ProcFireWall 之类的保护会拒绝首次打开: 提升特权后再试 */
            EnablePrivilege("SeDebugPrivilege");
            hProcess = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pids[i]);
        }
        if (hProcess) {
            if (TerminateProcess(hProcess, 0)) {
                found = TRUE;
                /* 别等满 3 秒: 驱动/看守进程可能卡在内核里, 这里只是"等它退干净",
                   超时就先处理下一个 (它已经是 Terminating 状态了)。 */
                if (WaitForSingleObject(hProcess, 1000) == WAIT_TIMEOUT)
                    SafeLog("[Process] pid %lu still terminating, moving on\r\n",
                            (unsigned long)pids[i]);
            }
            CloseHandle(hProcess);
        }
    }
    if (truncated)
        SafeLog("[Process] %s: more than 32 instances, only the first 32 handled\r\n",
                name);
    return found;
}

/* 一次快照收集所有在跑的课堂进程, 返回数量 */
/* 检查进程是否存在 */
BOOL IsProcessRunning(const char* name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return FALSE;
    }
    
    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    BOOL running = FALSE;
    
    if (Process32First(snapshot, &pe)) {
        do {
            if (_stricmp(pe.szExeFile, name) == 0) {
                running = TRUE;
                break;
            }
        } while (Process32Next(snapshot, &pe));
    }
    
    CloseHandle(snapshot);
    return running;
}

/* 检查驱动服务是否已安装 */
BOOL IsDriverInstalled(void) {
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) {
        return FALSE;
    }
    
    SC_HANDLE service = OpenService(scm, DRIVER_SERVICE_NAME, SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(scm);
        return FALSE;
    }
    
    SERVICE_STATUS status;
    QueryServiceStatus(service, &status);
    CloseServiceHandle(service);
    CloseServiceHandle(scm);
    
    return TRUE;
}

/* ========== 键盘解锁函数 ========== */

/* 查找 LockKeyboard.dll 的完整路径 */
static BOOL FindLockKeyboardDll(char* outPath, DWORD size) {
    return FindSoftwareFile("LockKeyboard.dll", outPath, size);
}

/* 通过 32 位 rundll32 代理调用 LockKeyboard.dll 的导出 (x64 进程兼容) */
static BOOL UnlockKeyboardViaRundll(HWND hwnd) {
    char dllPath[MAX_PATH];
    char params[MAX_PATH + 64];
    char sysDir[MAX_PATH];
    char rundll[MAX_PATH];
    SHELLEXECUTEINFOA sei;
    DWORD exitCode = 0;
    BOOL ok;

    if (!FindLockKeyboardDll(dllPath, sizeof(dllPath))) {
        if (hwnd) AppendLog(hwnd, "[Keyboard] LockKeyboard.dll not found\r\n");
        return FALSE;
    }

    /* 32 位系统上没有 SysWOW64; 有就优先用 (64 位进程需要 32 位代理) */
    sysDir[0] = '\0';
    if (GetSystemWow64DirectoryA(sysDir, sizeof(sysDir)) && sysDir[0]) {
        PathCombine(rundll, sysDir, "rundll32.exe");
    } else {
        GetSystemDirectoryA(sysDir, sizeof(sysDir));
        PathCombine(rundll, sysDir, "rundll32.exe");
    }

    snprintf(params, sizeof(params), "\"%s\",EnableKeyboard", dllPath);

    memset(&sei, 0, sizeof(sei));
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpFile = rundll;                                  /* 32-bit loader */
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;

    ok = ShellExecuteExA(&sei);
    if (ok) {
        /* ★ hProcess 可能是 NULL (DDE/外壳代理), GetExitCodeProcess 也可能失败:
           以前 exitCode 初值 0, 这两种情况都会把"什么都没执行"报成"解锁成功"。 */
        ok = FALSE;
        if (sei.hProcess) {
            DWORD wait = WaitForSingleObject(sei.hProcess, 5000);
            exitCode = (DWORD)-1;
            if (wait == WAIT_OBJECT_0 &&
                GetExitCodeProcess(sei.hProcess, &exitCode) && exitCode == 0)
                ok = TRUE;
            if (wait == WAIT_TIMEOUT) exitCode = 259;   /* STILL_ACTIVE */
            CloseHandle(sei.hProcess);
        }
    }
    if (hwnd)
        AppendLog(hwnd, "[Keyboard] rundll32 proxy EnableKeyboard -> exit=%ld\r\n",
                  (long)exitCode);
    return ok;
}

/* 通过厂商DLL解锁键盘 (最可靠) */
static BOOL UnlockKeyboardByDll(HWND hwnd) {
    char dllPath[MAX_PATH];
    HMODULE hDll = NULL;

    if (!FindLockKeyboardDll(dllPath, sizeof(dllPath))) {
        AppendLog(hwnd, "[Keyboard] LockKeyboard.dll not found in the install dirs\r\n");
        return FALSE;
    }

    AppendLog(hwnd, "[Keyboard] Found DLL: %s\r\n", dllPath);
    hDll = LoadLibraryA(dllPath);
    if (!hDll) {
        DWORD err = GetLastError();
        AppendLog(hwnd, "[Keyboard] Cannot load %s (Error: %lu)\r\n", dllPath, err);
        if (err == 193 /*ERROR_BAD_EXE_FORMAT*/) {
            /* 32 位 DLL 无法载入 x64 进程 -> 交给 32 位 rundll32 执行 */
            AppendLog(hwnd,
                "[Keyboard] 32-bit DLL + x64 process, using rundll32 proxy\r\n");
            return UnlockKeyboardViaRundll(hwnd);
        }
        /* ★ 不再退化成 LoadLibraryA("LockKeyboard.dll"):
           提权进程的默认搜索顺序包含当前工作目录 / PATH, 普通用户在那些位置
           放一个同名 DLL 就能在本进程里执行任意代码。只认上面找到的绝对路径。 */
        return FALSE;
    }

    {
        typedef void (WINAPI *EnableKeyboardFunc)(void);
        EnableKeyboardFunc EnableKeyboard = (EnableKeyboardFunc)GetProcAddress(hDll, "EnableKeyboard");
        if (!EnableKeyboard) {
            AppendLog(hwnd, "[Keyboard] Cannot find EnableKeyboard export\r\n");
            FreeLibrary(hDll);
            return FALSE;
        }

        AppendLog(hwnd, "[Keyboard] Calling EnableKeyboard()...\r\n");
        EnableKeyboard();
        AppendLog(hwnd, "[Keyboard] EnableKeyboard() executed\r\n");

        FreeLibrary(hDll);
    }
    return TRUE;
}

/* ------------------------------------------------------------------
 * 打开键盘类设备: 完全复刻厂商 LockKeyboard.dll 的做法
 *   DefineDosDeviceW(DDD_RAW_TARGET_PATH, L"Kbd", <NT设备路径>)
 *   CreateFileW(L"\\\\.\\Kbd", GENERIC_WRITE, FILE_SHARE_READ|WRITE, NULL, OPEN_EXISTING, 0, NULL)
 * 注意: 必须先 DefineDosDevice 建立 "Kbd" 链接, 再打开 \\.\Kbd;
 *       旧版代码建完链接却去开 \\.\OeKbdFilter, 那段是死代码。
 * ------------------------------------------------------------------ */
static HANDLE OpenKeyboardClassDevice(const wchar_t* ntTarget, DWORD* pErrOut) {
    HANDLE hDevice;
    DWORD  err = 0;

    DefineDosDeviceW(DDD_REMOVE_DEFINITION, DEVICE_KBD_DOSNAME, NULL);

    if (!DefineDosDeviceW(DDD_RAW_TARGET_PATH, DEVICE_KBD_DOSNAME, ntTarget)) {
        err = GetLastError();
        hDevice = INVALID_HANDLE_VALUE;
    } else {
        hDevice = CreateFileW(L"\\\\.\\Kbd", GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL, OPEN_EXISTING, 0, NULL);
        if (hDevice == INVALID_HANDLE_VALUE) {
            err = GetLastError();
        }
    }

    if (pErrOut) *pErrOut = err;
    return hDevice;
}

/* 打开驱动自己的控制设备 \\.\OeKbdFilter (KbFilter 的 IoCreateDevice 产物) */
static HANDLE OpenKbFilterControlDevice(DWORD* pErrOut) {
    HANDLE h = CreateFileA(DEVICE_KBD_LINK, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (pErrOut) *pErrOut = (h == INVALID_HANDLE_VALUE) ? GetLastError() : 0;
    return h;
}

/* 发送键盘控制码; 成功 TRUE。ntTarget 为 NULL 时改用 \\.\OeKbdFilter */
static BOOL SendKeyboardIoctl(const wchar_t* ntTarget, DWORD code, DWORD* pErrOut) {
    HANDLE hDevice;
    DWORD  err = 0, bytes = 0;
    BOOL   ok = FALSE;

    hDevice = ntTarget ? OpenKeyboardClassDevice(ntTarget, &err)
                       : OpenKbFilterControlDevice(&err);

    if (hDevice == INVALID_HANDLE_VALUE) {
        if (pErrOut) *pErrOut = err;
        return FALSE;
    }

    ok = DeviceIoControl(hDevice, code, NULL, 0, NULL, 0, &bytes, NULL);
    if (!ok && pErrOut) *pErrOut = GetLastError();

    CloseHandle(hDevice);
    if (ntTarget) DefineDosDeviceW(DDD_REMOVE_DEFINITION, DEVICE_KBD_DOSNAME, NULL);
    return ok;
}

/* 清理可能残留的 "Kbd" DOS 设备链接 */
static void CleanupKbdDosDevice(void) {
    DefineDosDeviceW(DDD_REMOVE_DEFINITION, DEVICE_KBD_DOSNAME, NULL);
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                     DEVICE_KBD_DOSNAME, L"\\Device\\KeyboardClass0");
    DefineDosDeviceW(DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE,
                     DEVICE_KBD_DOSNAME, L"\\Device\\KeyboardClass1");
}

/* 通过 IOCTL 解锁键盘 (首选方案: 不依赖厂商 DLL / rundll32, x64 原生可用) */
static BOOL UnlockKeyboardByIoctl(HWND hwnd) {
    BOOL ok0 = FALSE, ok1 = FALSE, okd = FALSE;
    DWORD e0 = 0, e1 = 0, ed = 0;

    /* KeyboardClass0 / KeyboardClass1: 与厂商 EnableKeyboard 完全一致的两次调用 */
    ok0 = SendKeyboardIoctl(L"\\Device\\KeyboardClass0", IOCTL_KEYBOARD_ALL_RELEASE, &e0);
    AppendLog(hwnd, "[Keyboard] IOCTL 0x%X -> KeyboardClass0: %s (err=%lu)\r\n",
              IOCTL_KEYBOARD_ALL_RELEASE, ok0 ? "OK" : "FAIL", e0);

    ok1 = SendKeyboardIoctl(L"\\Device\\KeyboardClass1", IOCTL_KEYBOARD_ALL_RELEASE, &e1);
    AppendLog(hwnd, "[Keyboard] IOCTL 0x%X -> KeyboardClass1: %s (err=%lu)\r\n",
              IOCTL_KEYBOARD_ALL_RELEASE, ok1 ? "OK" : "FAIL", e1);

    if (ok0 || ok1) {
        g_workingIoctl = IOCTL_KEYBOARD_ALL_RELEASE;
        CleanupKbdDosDevice();
        return TRUE;
    }

    /* 备用: 驱动自身控制设备 \\.\OeKbdFilter */
    okd = SendKeyboardIoctl(NULL, IOCTL_KEYBOARD_ALL_RELEASE, &ed);
    AppendLog(hwnd, "[Keyboard] IOCTL 0x%X -> \\\\.\\OeKbdFilter: %s (err=%lu)\r\n",
              IOCTL_KEYBOARD_ALL_RELEASE, okd ? "OK" : "FAIL", ed);
    if (okd) g_workingIoctl = IOCTL_KEYBOARD_ALL_RELEASE;

    CleanupKbdDosDevice();
    return okd;
}

/* 解锁键盘 - 主函数
   顺序已调整: 先走"实证 IOCTL"(x64 原生, 无外部依赖, 也不会拉起 rundll32),
   失败后才退回厂商 LockKeyboard.dll / 32 位 rundll32 代理。 */
BOOL UnlockKeyboard(HWND hwnd) {
    AppendLog(hwnd, "\r\n[Keyboard] Attempting to unlock keyboard...\r\n");
    UpdateStatus(hwnd, "Status: Unlocking keyboard...");

    BOOL success = FALSE;

    /* 方法1: 直接 IOCTL 0xB2008 -> \Device\KeyboardClass0/1 (与厂商 DLL 行为一致) */
    AppendLog(hwnd, "[Keyboard] Method 1: native IOCTL 0x%X (verified)\r\n",
              IOCTL_KEYBOARD_ALL_RELEASE);
    success = UnlockKeyboardByIoctl(hwnd);

    /* 方法2: 厂商 DLL (仅当 IOCTL 不可用时; 32 位 DLL 会自动转 rundll32) */
    if (!success) {
        AppendLog(hwnd, "[Keyboard] Method 2: vendor LockKeyboard.dll\r\n");
        success = UnlockKeyboardByDll(hwnd);
    }

    if (success) {
        AppendLog(hwnd, "[Keyboard] Keyboard unlocked successfully!\r\n");
        UpdateStatus(hwnd, "Status: Keyboard unlocked");
    } else {
        AppendLog(hwnd, "[Keyboard] ERROR: All unlock methods failed\r\n");
        UpdateStatus(hwnd, "Status: Keyboard unlock failed");
    }

    return success;
}

/* ========== 黑屏解除函数 ========== */

/* 切换到默认桌面 */
/* 读取当前输入桌面 (input desktop) 名称 */
static BOOL GetInputDesktopName(char* out, DWORD size) {
    HDESK hIn = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    BOOL  ok = FALSE;

    if (out && size) out[0] = '\0';
    if (hIn) {
        DWORD needed = 0;
        ok = GetUserObjectInformationA(hIn, UOI_NAME, out, size, &needed) ? TRUE : FALSE;
        CloseDesktop(hIn);
    }
    return ok;
}

/* 判断当前是否处于"锁定桌面" (实证: systemoper.dll 用 MCLOCK Desktop) */
static BOOL IsLockDesktopActive(void) {
    char name[128];
    if (!GetInputDesktopName(name, sizeof(name))) return FALSE;
    return (_stricmp(name, LOCK_DESKTOP_NAME) == 0);
}

/* ========== 进程终止函数 ========== */

BOOL KillControlProcesses(HWND hwnd) {
    AppendLog(hwnd, "\r\n[Process] Killing control processes...\r\n");
    UpdateStatus(hwnd, "Status: Terminating processes...");
    
    int killed = 0;
    
    for (int i = 0; TARGET_PROCESSES[i]; i++) {
        AppendLog(hwnd, "[Process] Checking %s...\r\n", TARGET_PROCESSES[i]);
        
        if (IsProcessRunning(TARGET_PROCESSES[i])) {
            AppendLog(hwnd, "[Process] Terminating %s...\r\n", TARGET_PROCESSES[i]);
            if (KillProcessByName(TARGET_PROCESSES[i])) {
                AppendLog(hwnd, "[Process] %s terminated\r\n", TARGET_PROCESSES[i]);
                killed++;
            } else {
                AppendLog(hwnd, "[Process] WARNING: Failed to terminate %s\r\n", TARGET_PROCESSES[i]);
            }
        } else {
            AppendLog(hwnd, "[Process] %s not running\r\n", TARGET_PROCESSES[i]);
        }
    }
    
    AppendLog(hwnd, "[Process] Terminated %d processes\r\n", killed);
    UpdateStatus(hwnd, "Status: Processes terminated");
    
    return (killed > 0);
}

/* ========== 驱动卸载函数 ========== */

/* 停止依赖该驱动的服务: 直接 ControlService(STOP) 在有依赖时会失败
   (ERROR_SERVICE_DEPENDENCIES: 1051), 必须先停掉依赖方。 */
static BOOL StopDependentServices(SC_HANDLE scm, HWND hwnd, const char* driverName) {
    SC_HANDLE svc = OpenServiceA(scm, driverName,
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    DWORD needed = 0, count = 0;
    LPENUM_SERVICE_STATUSA deps = NULL;

    if (!svc) return FALSE;

    EnumDependentServicesA(svc, SERVICE_ACTIVE, NULL, 0, &needed, &count);
    if (needed == 0) {
        CloseServiceHandle(svc);
        return TRUE;
    }

    deps = (LPENUM_SERVICE_STATUSA)malloc(needed);
    if (deps && EnumDependentServicesA(svc, SERVICE_ACTIVE, deps, needed, &needed, &count)) {
        for (DWORD i = 0; i < count; i++) {
            SC_HANDLE dep = OpenServiceA(scm, deps[i].lpServiceName,
                                         SERVICE_STOP | SERVICE_QUERY_STATUS);
            if (dep) {
                SERVICE_STATUS st;
                if (ControlService(dep, SERVICE_CONTROL_STOP, &st))
                    AppendLog(hwnd, "[Driver] dependent service '%s' stopped\r\n",
                              deps[i].lpServiceName);
                CloseServiceHandle(dep);
            }
        }
        if (count) Sleep(500);
    }
    if (deps) free(deps);
    CloseServiceHandle(svc);
    return TRUE;
}

/* KbFilter 是作为"键盘类过滤驱动"安装的 (UpperFilters)。
   只删服务不删 UpperFilters, 重启后系统仍会尝试加载它。
   键盘类 GUID: {4D36E96B-E325-11CE-BFC1-08002BE10318} */
static BOOL RemoveKeyboardClassFilterEntry(HWND hwnd, const char* filterName) {
    static const char* kbdClassKey =
        "SYSTEM\\CurrentControlSet\\Control\\Class\\{4D36E96B-E325-11CE-BFC1-08002BE10318}";
    static const char* valueNames[] = { "UpperFilters", "LowerFilters", NULL };
    BOOL touched = FALSE;

    for (int v = 0; valueNames[v]; v++) {
        HKEY hKey = NULL;
        DWORD type = 0, size = 0;
        BYTE* buf = NULL;

        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kbdClassKey, 0,
                KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) != ERROR_SUCCESS) {
            continue;
        }

        if (RegQueryValueExA(hKey, valueNames[v], NULL, &type, NULL, &size) == ERROR_SUCCESS &&
            (type == REG_MULTI_SZ || type == REG_SZ) && size > 1) {
            buf = (BYTE*)malloc(size);
            if (buf && RegQueryValueExA(hKey, valueNames[v], NULL, &type, buf, &size) == ERROR_SUCCESS) {
                char* p   = (char*)buf;
                char* end = (char*)buf + size;
                BYTE* out = (BYTE*)calloc(1, size + 2);
                DWORD outLen = 0;

                if (out) {
                    BOOL removedOne = FALSE;

                    while (p < end && *p) {
                        size_t l = strlen(p);
                        if (_stricmp(p, filterName) != 0) {
                            memcpy((char*)out + outLen, p, l + 1);
                            outLen += (DWORD)(l + 1);
                        } else {
                            AppendLog(hwnd, "[Driver] removing '%s' from keyboard class %s\r\n",
                                      filterName, valueNames[v]);
                            removedOne = TRUE;
                        }
                        p += l + 1;
                    }
                    ((char*)out)[outLen] = '\0';

                    /* ★ 没删到东西就一个字节都不写: 以前无论有没有命中都会
                       RegSetValueEx, 还把 REG_SZ 顺手改写成 REG_MULTI_SZ ——
                       对启动关键键做无意义写入。 */
                    if (!removedOne) {
                        free(out);
                    } else if (outLen > 0) {
                        /* 标准语义: UpperFilters/LowerFilters 是"字符串列表"。
                           只剩一项且原来是 REG_SZ 时按 SZ 写回; 剩下多项必须是
                           MULTI_SZ —— 用 REG_SZ 写多串会在第一个 NUL 截断, 有的
                           解析器又会当 MULTI_SZ 读, 行为不一致。 */
                        DWORD outType = (type == REG_SZ &&
                                         outLen == (DWORD)(strlen((char*)out) + 1))
                                        ? REG_SZ : REG_MULTI_SZ;
                        DWORD cb = (outType == REG_SZ)
                                   ? (DWORD)(strlen((char*)out) + 1)
                                   : outLen + 1;
                        if (RegSetValueExA(hKey, valueNames[v], 0, outType,
                                           out, cb) == ERROR_SUCCESS) {
                            AppendLog(hwnd, "[Driver] %s updated\r\n", valueNames[v]);
                            touched = TRUE;
                        }
                        free(out);
                    } else {
                        if (RegDeleteValueA(hKey, valueNames[v]) == ERROR_SUCCESS) {
                            AppendLog(hwnd, "[Driver] %s deleted (was only entry)\r\n", valueNames[v]);
                            touched = TRUE;
                        }
                        free(out);
                    }
                }
            }
        }
        if (buf) free(buf);
        RegCloseKey(hKey);
    }
    return touched;
}

/* 停止并删除单个驱动服务 */
static BOOL RemoveOneDriver(SC_HANDLE scm, HWND hwnd, const char* driverName) {
    BOOL result = FALSE;

    /* 先停依赖方, 再停自己 */
    StopDependentServices(scm, hwnd, driverName);

    /* 停止服务 */
    SC_HANDLE service = OpenService(scm, driverName,
                                    SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (service) {
        SERVICE_STATUS status;
        if (ControlService(service, SERVICE_CONTROL_STOP, &status)) {
            /* 等待真正进入 STOPPED, 否则 DeleteService 会失败 */
            for (int i = 0; i < 20; i++) {
                QueryServiceStatus(service, &status);
                if (status.dwCurrentState == SERVICE_STOPPED) break;
                Sleep(250);
            }
            /* ★ 只有真的停了才报 stopped: 20 次轮询后仍是 RUNNING 的话,
               驱动还在内存里继续锁键盘, 日志不能替它宣布成功。 */
            if (status.dwCurrentState == SERVICE_STOPPED)
                AppendLog(hwnd, "[Driver] %s stopped\r\n", driverName);
            else
                AppendLog(hwnd, "[Driver] WARNING: %s still state=%lu after 5s\r\n",
                          driverName, (unsigned long)status.dwCurrentState);
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_SERVICE_NOT_ACTIVE) {
                AppendLog(hwnd, "[Driver] %s already stopped\r\n", driverName);
            } else {
                AppendLog(hwnd, "[Driver] %s stop failed (Error: %lu)\r\n", driverName, err);
            }
        }
        CloseServiceHandle(service);
    } else {
        return FALSE;
    }

    /* 删除服务 */
    service = OpenService(scm, driverName, DELETE);
    if (service) {
        SetLastError(ERROR_SUCCESS);
        if (DeleteService(service)) {
            AppendLog(hwnd, "[Driver] %s service deleted (driver file unloads on reboot)\r\n",
                      driverName);
            result = TRUE;
        } else {
            DWORD err = GetLastError();
            if (err == ERROR_SERVICE_MARKED_FOR_DELETE) {
                AppendLog(hwnd, "[Driver] %s already marked for delete\r\n", driverName);
                result = TRUE;
            } else {
                AppendLog(hwnd, "[Driver] %s delete failed (Error: %lu)\r\n", driverName, err);
            }
        }
        CloseServiceHandle(service);
    }

    return result;
}

BOOL UninstallDriver(HWND hwnd) {
    AppendLog(hwnd, "\r\n[Driver] Uninstalling keyboard filter drivers...\r\n");
    UpdateStatus(hwnd, "Status: Uninstalling drivers...");
    
    BOOL success = FALSE;
    int removed = 0;
    
    /* 打开服务管理器 */
    SC_HANDLE scm = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        AppendLog(hwnd, "[Driver] ERROR: Cannot open SCM (Error: %d)\r\n", GetLastError());
        AppendLog(hwnd, "[Driver] Trying DriverInstall.exe alternative...\r\n");
        
        /* 优先尝试 DriverInstall.exe (用已有的安装目录探测, 不再硬编码 Administrator 路径) */
        char cmdLine[MAX_PATH] = {0};
        if (!FindSoftwareFile("DriverInstall.exe", cmdLine, sizeof(cmdLine))) {
            const char* possiblePaths[] = {
                ".\\DriverInstall.exe",
                "..\\DriverInstall.exe",
                NULL
            };
            for (int i = 0; possiblePaths[i]; i++) {
                if (GetFileAttributes(possiblePaths[i]) != INVALID_FILE_ATTRIBUTES) {
                    GetFullPathName(possiblePaths[i], MAX_PATH, cmdLine, NULL);
                    break;
                }
            }
        }
        
        if (cmdLine[0] == '\0') {
            AppendLog(hwnd, "[Driver] DriverInstall.exe not found\r\n");
            UpdateStatus(hwnd, "Status: Driver uninstall failed");
            return FALSE;
        }
        
        /* 与厂商脚本一致: DriverInstall.exe /S /DIR=<自己的目录>\ /OPT=uninst /d1=KbFilter */
        char installDir[MAX_PATH];
        strncpy(installDir, cmdLine, sizeof(installDir) - 1);
        installDir[sizeof(installDir) - 1] = '\0';
        PathRemoveFileSpec(installDir);

        char args[MAX_PATH * 2];
        snprintf(args, sizeof(args), "/S /DIR=%s\\ /OPT=uninst /d1=%s",
                 installDir, DRIVER_SERVICE_NAME);
        
        AppendLog(hwnd, "[Driver] Running: %s %s\r\n", cmdLine, args);
        
        SHELLEXECUTEINFO sei = {0};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS;
        sei.lpVerb = "runas";
        sei.lpFile = cmdLine;
        sei.lpParameters = args;
        sei.nShow = SW_HIDE;
        
        if (ShellExecuteEx(&sei)) {
            /* ★ hProcess 可能是 NULL (外壳代理), GetExitCodeProcess 也可能失败:
               以前 exitCode 未初始化, 这两种情况会把成败判成栈垃圾。 */
            DWORD exitCode = (DWORD)-1;

            success = FALSE;
            if (sei.hProcess) {
                DWORD wait = WaitForSingleObject(sei.hProcess, 10000);
                if (wait == WAIT_OBJECT_0 &&
                    GetExitCodeProcess(sei.hProcess, &exitCode) && exitCode == 0)
                    success = TRUE;
                if (wait == WAIT_TIMEOUT) exitCode = 259;   /* STILL_ACTIVE */
                CloseHandle(sei.hProcess);
            }
            AppendLog(hwnd, "[Driver] DriverInstall returned: %ld\r\n", (long)exitCode);
        }
        
        AppendLog(hwnd, "[Driver] A system restart may be required\r\n");
        UpdateStatus(hwnd, "Status: Driver uninstall requested");
        return success;
    }
    
    /* 逐个卸载过滤驱动 */
    for (int i = 0; FILTER_DRIVERS[i]; i++) {
        AppendLog(hwnd, "[Driver] Processing %s...\r\n", FILTER_DRIVERS[i]);
        if (RemoveOneDriver(scm, hwnd, FILTER_DRIVERS[i])) {
            removed++;
            success = TRUE;
        }
    }
    
    CloseServiceHandle(scm);

    /* 关键补充: 删掉服务还不够, 必须同时从键盘类过滤器列表里移除 KbFilter,
       否则重启后系统仍会加载该驱动。 */
    if (RemoveKeyboardClassFilterEntry(hwnd, DRIVER_SERVICE_NAME)) {
        AppendLog(hwnd, "[Driver] keyboard class filter entry cleaned\r\n");
        success = TRUE;
    }
    
    if (success) {
        AppendLog(hwnd, "[Driver] Uninstalled %d driver(s) successfully!\r\n", removed);
        AppendLog(hwnd, "[Driver] A system restart may be required\r\n");
        UpdateStatus(hwnd, "Status: Drivers uninstalled");
    } else {
        AppendLog(hwnd, "[Driver] No drivers uninstalled (may not be installed)\r\n");
        UpdateStatus(hwnd, "Status: No drivers to uninstall");
    }
    
    return success;
}

/* ========== 配置修改函数 ========== */

/* 已发现的软件安装目录缓存 */
static char g_installDir[MAX_PATH] = {0};
static volatile LONG g_installLock = 0;

/* g_installDir 会被 UI 线程和小窗线程并发读写 (内容幂等, 但可能有半写状态) */
static void SetInstallDir(const char* dir) {
    while (InterlockedExchange(&g_installLock, 1)) Sleep(0);
    strncpy(g_installDir, dir, MAX_PATH - 1);
    g_installDir[MAX_PATH - 1] = '\0';
    InterlockedExchange(&g_installLock, 0);
}

static BOOL GetInstallDir(char* out, DWORD size) {
    BOOL ok = FALSE;

    if (!out || size < 2) return FALSE;         /* 以前 size==0 会写 out[-1] */
    while (InterlockedExchange(&g_installLock, 1)) Sleep(0);
    if (g_installDir[0]) {
        snprintf(out, size, "%s", g_installDir);
        out[size - 1] = '\0';
        ok = TRUE;
    }
    InterlockedExchange(&g_installLock, 0);
    return ok;
}

/* 从进程句柄获取其可执行文件完整路径 */
static BOOL GetProcessPath(DWORD pid, char* outPath, DWORD size) {
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) return FALSE;

    DWORD len = size;
    BOOL ok = QueryFullProcessImageName(hProcess, 0, outPath, &len);
    CloseHandle(hProcess);
    return ok;
}

/* 从运行中的控制进程反推软件安装目录 */
static BOOL FindDirFromProcesses(char* outDir, DWORD size) {
    static const char* procNames[] = {
        "Student.exe", "MmcStudent.exe", "Teacher.exe", "BlackSlient.exe",
        "MultiClient.exe", "ScreenSender.exe", "ScreenRender.exe",
        "DriverInstall.exe", "Ctsc_Multi.exe", NULL
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return FALSE;

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(pe);
    BOOL found = FALSE;

    if (Process32First(snapshot, &pe)) {
        do {
            for (int i = 0; procNames[i]; i++) {
                if (_stricmp(pe.szExeFile, procNames[i]) == 0) {
                    char procPath[MAX_PATH];
                    if (GetProcessPath(pe.th32ProcessID, procPath, MAX_PATH)) {
                        PathRemoveFileSpec(procPath);
                        snprintf(outDir, size, "%s", procPath);
                        outDir[size - 1] = '\0';
                        found = TRUE;
                        break;
                    }
                }
            }
            if (found) break;
        } while (Process32Next(snapshot, &pe));
    }

    CloseHandle(snapshot);
    return found;
}

/* 从注册表卸载项查询安装目录 */
/* 注册表里的安装目录经常带引号 / 尾反斜杠 / %ProgramFiles% 环境变量。
   直接拿来用会让之后**所有** FindSoftwareFile 失效, 而且它会被缓存住
   (一条脏值能把这个会话里所有 DLL/EXE 查找全废掉), 所以这里统一清洗 +
   校验目录真的还在。 */
static BOOL CleanInstallDir(const char* raw, char* out, DWORD size) {
    char   tmp[MAX_PATH * 2];
    char*  p;
    size_t n;

    if (!raw || !raw[0] || !out || size < 3) return FALSE;

    if (strchr(raw, '%')) {
        if (!ExpandEnvironmentStringsA(raw, tmp, sizeof(tmp))) return FALSE;
    } else {
        _snprintf(tmp, sizeof(tmp) - 1, "%s", raw);
        tmp[sizeof(tmp) - 1] = '\0';
    }

    p = tmp;
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    n = strlen(p);
    while (n && (p[n - 1] == '"' || p[n - 1] == ' ' || p[n - 1] == '\t' ||
                 p[n - 1] == '\\'))
        p[--n] = '\0';
    if (!n || n >= size) return FALSE;
    if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES) return FALSE;

    memcpy(out, p, n + 1);
    return TRUE;
}

static BOOL FindDirFromRegistry(char* outDir, DWORD size) {
    /* 优先查询卸载项 (含完整卸载路径) */
    static const char* uninstallRoots[] = {
        "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
    };

    for (int r = 0; r < 2; r++) {
        HKEY hRoot;
        if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, uninstallRoots[r], 0,
                KEY_READ | KEY_WOW64_64KEY, &hRoot) != ERROR_SUCCESS) {
            if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, uninstallRoots[r], 0,
                    KEY_READ, &hRoot) != ERROR_SUCCESS) {
                continue;
            }
        }

        DWORD subCount = 0;
        RegQueryInfoKey(hRoot, NULL, NULL, NULL, &subCount, NULL, NULL,
            NULL, NULL, NULL, NULL, NULL);

        for (DWORD i = 0; i < subCount; i++) {
            char subName[256];
            DWORD subLen = sizeof(subName);
            if (RegEnumKeyEx(hRoot, i, subName, &subLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                continue;

            /* 匹配 Os-Easy 教室软件 (不同版本 Uninstall 键名不完全一致, 用包含判断) */
            if (!strstr(subName, "Os-Easy") && !strstr(subName, "os-easy") &&
                !strstr(subName, "multicast teaching")) {
                continue;
            }

            HKEY hSub;
            if (RegOpenKeyEx(hRoot, subName, 0, KEY_READ, &hSub) != ERROR_SUCCESS)
                continue;

            /* 读 UninstallString 或 InstallLocation */
            char buf[MAX_PATH * 2] = {0};
            DWORD bufSize = sizeof(buf);
            DWORD type = 0;

            if (RegQueryValueEx(hSub, "InstallLocation", NULL, &type,
                    (LPBYTE)buf, &bufSize) == ERROR_SUCCESS && buf[0] != '\0' &&
                CleanInstallDir(buf, outDir, size)) {
                RegCloseKey(hSub);
                RegCloseKey(hRoot);
                return TRUE;
            }

            bufSize = sizeof(buf);
            if (RegQueryValueEx(hSub, "UninstallString", NULL, &type,
                    (LPBYTE)buf, &bufSize) == ERROR_SUCCESS && buf[0] != '\0') {
                /* 去掉文件名, 保留目录 */
                char* p1 = strrchr(buf, '\\');
                if (p1) *p1 = '\0';
                if (CleanInstallDir(buf, outDir, size)) {
                    RegCloseKey(hSub);
                    RegCloseKey(hRoot);
                    return TRUE;
                }
            }

            RegCloseKey(hSub);
        }
        RegCloseKey(hRoot);
    }
    return FALSE;
}

/* 动态定位软件安装目录 */
static BOOL FindInstallDir(char* outDir, DWORD size) {
    /* 缓存命中 (加锁读, 避免读到半写状态) */
    if (GetInstallDir(outDir, size)) {
        return TRUE;
    }

    /* 1. 注册表卸载项 (最权威) */
    if (FindDirFromRegistry(outDir, size)) {
        SetInstallDir(outDir);
        return TRUE;
    }

    /* 2. 运行中的控制进程 */
    if (FindDirFromProcesses(outDir, size)) {
        SetInstallDir(outDir);
        return TRUE;
    }

    /* 3. 标准安装路径 */
    static const char* defaultDirs[] = {
        "C:\\Program Files (x86)\\Os-Easy\\os-easy multicast teaching system",
        "C:\\Program Files\\Os-Easy\\os-easy multicast teaching system",
        NULL
    };
    for (int i = 0; defaultDirs[i]; i++) {
        if (GetFileAttributes(defaultDirs[i]) != INVALID_FILE_ATTRIBUTES) {
            strncpy(outDir, defaultDirs[i], size - 1);
            outDir[size - 1] = '\0';
            SetInstallDir(outDir);
            return TRUE;
        }
    }

    return FALSE;
}

/* 查找软件目录下的文件 (动态定位 + 相对路径搜索) */
static BOOL FindSoftwareFile(const char* relName, char* outPath, DWORD size) {
    char searchPaths[10][MAX_PATH];
    int n = 0;
    char exeDir[MAX_PATH];
    char installDir[MAX_PATH];

    /* 找不到时也必须给调用方一个确定值: 之前不改 outPath, 调用方拿着
       未初始化的栈缓冲去当路径用 (可能连 NUL 都没有 -> 越界读)。 */
    if (!outPath || size < 2) return FALSE;
    outPath[0] = '\0';

    if (!GetModuleFileName(NULL, exeDir, MAX_PATH)) return FALSE;
    exeDir[MAX_PATH - 1] = '\0';
    PathRemoveFileSpec(exeDir);

    /* 1. 动态定位的安装目录 (核心) */
    if (FindInstallDir(installDir, sizeof(installDir))) {
        _snprintf(searchPaths[n], MAX_PATH - 1, "%s\\%s", installDir, relName);
        searchPaths[n][MAX_PATH - 1] = '\0';
        n++;
        _snprintf(searchPaths[n], MAX_PATH - 1, "%s\\x64\\%s", installDir, relName);
        searchPaths[n][MAX_PATH - 1] = '\0';
        n++;
    }

    /* 2. exe 同级 */
    _snprintf(searchPaths[n], MAX_PATH - 1, "%s\\%s", exeDir, relName);
    searchPaths[n][MAX_PATH - 1] = '\0';
    n++;

    /* 3. 当前工作目录 */
    _snprintf(searchPaths[n], MAX_PATH - 1, "%s", relName);
    searchPaths[n][MAX_PATH - 1] = '\0';
    n++;

    for (int i = 0; i < n; i++) {
        if (GetFileAttributes(searchPaths[i]) != INVALID_FILE_ATTRIBUTES) {
            strncpy(outPath, searchPaths[i], size - 1);
            outPath[size - 1] = '\0';
            return TRUE;
        }
    }
    return FALSE;
}

/* 大小写不敏感的子串查找 */
static const char* _stristr(const char* hay, const char* needle) {
    size_t nlen = strlen(needle);
    if (!nlen) return hay;
    for (; *hay; hay++) {
        if (_strnicmp(hay, needle, nlen) == 0) return hay;
    }
    return NULL;
}

/* 读取 core.conf 中 "/Key/Value/" 的 Value (空值返回空串) */
static BOOL ReadConfigValue(const char* path, const char* key, char* out, size_t outSize) {
    FILE*  f;
    char   buf[512];
    size_t klen = strlen(key);
    BOOL   found = FALSE;

    if (out && outSize) out[0] = '\0';
    f = fopen(path, "rb");
    if (!f) return FALSE;

    while (fgets(buf, sizeof(buf), f)) {
        char* p = buf;
        char* e;
        while (*p == ' ' || *p == '\t') p++;
        if (*p++ != '/') continue;
        if (_strnicmp(p, key, klen) != 0) continue;
        p += klen;
        if (*p++ != '/') continue;
        e = strchr(p, '/');
        if (!e) continue;
        *e = '\0';
        if (out && outSize) {
            strncpy(out, p, outSize - 1);
            out[outSize - 1] = '\0';
        }
        found = TRUE;
        break;
    }
    fclose(f);
    return found;
}

/* ------------------------------------------------------------------
 * 自启动项清除
 * 实证: Startup.exe 是一个"通用 Run 键写入器"
 *   RegOpenKeyExA("Software\\Microsoft\\Windows\\CurrentVersion\\Run")
 *   + RegSetValueExA / RegDeleteValueA (键名与命令行由调用者传入)
 * 所以这里不写死键名, 而是扫描各 Run/RunOnce 的"值数据",
 * 命中教室软件相关进程名即删除 —— 比猜键名可靠。
 * ------------------------------------------------------------------ */
/* Run 值里"提到某个进程"必须按路径分段精确判断。
   以前直接 _stristr(data, "Student.exe"): "C:\x\MyStudent.exe"、
   "x64\Student.exe.bak" 也会命中 —— 删自启动项是不可恢复的, 不能靠子串。 */
static BOOL DataMentionsProcess(const char* data, const char* name)
{
    const char* p;
    size_t      nlen;

    if (!data || !name || !(nlen = strlen(name))) return FALSE;
    p = data;
    while ((p = _stristr(p, name)) != NULL) {
        char before = (p == data) ? '\0' : p[-1];
        char after  = p[nlen];

        if ((before == '\0' || before == '\\' || before == '"' ||
             before == ' '  || before == '\'') &&
            (after == '\0' || after == '"' || after == ' ' || after == '\'' ||
             after == ',' || after == '\r' || after == '\n'))
            return TRUE;
        p += nlen;
    }
    return FALSE;
}

static BOOL RemoveStartupPersistence(HWND hwnd) {
    /* ★ HKLM 必须**两种视图各扫一遍**: 32 位进程访问 HKLM\Software 会被 WOW64
       重定向到 WOW6432Node, 所以以前那两行手写的 "WOW6432Node\..." 在 32 位
       构建里等于把同一个键扫了两遍, 真正的 64 位 Run/RunOnce 永远漏掉 ——
       看守进程就是这样把 Student.exe 拉回来的。 */
    struct { HKEY root; const char* sub; REGSAM view; } roots[] = {
        { HKEY_CURRENT_USER,  "Software\\Microsoft\\Windows\\CurrentVersion\\Run",      0 },
        { HKEY_CURRENT_USER,  "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",  0 },
        { HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
          KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
          KEY_WOW64_64KEY },
        { HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
          KEY_WOW64_32KEY },
        { HKEY_LOCAL_MACHINE, "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce",
          KEY_WOW64_32KEY },
    };
    int removed = 0;

    for (int r = 0; r < (int)(sizeof(roots) / sizeof(roots[0])); r++) {
        HKEY hKey = NULL;
        DWORD subCount = 0;

        if (RegOpenKeyExA(roots[r].root, roots[r].sub, 0,
                          KEY_READ | KEY_SET_VALUE | roots[r].view, &hKey) != ERROR_SUCCESS) {
            continue;
        }
        RegQueryInfoKeyA(hKey, NULL, NULL, NULL, NULL, NULL, NULL,
                         &subCount, NULL, NULL, NULL, NULL);

        for (DWORD i = 0; i < subCount; i++) {
            char  name[512];
            char  data[2048];
            DWORD nameLen = sizeof(name), dataLen = sizeof(data), type = 0;
            BOOL  match = FALSE;

            if (RegEnumValueA(hKey, i, name, &nameLen, NULL, &type,
                              (LPBYTE)data, &dataLen) != ERROR_SUCCESS) {
                continue;
            }
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            data[sizeof(data) - 1] = '\0';

            for (int n = 0; STARTUP_PROCESS_NAMES[n]; n++) {
                if (DataMentionsProcess(data, STARTUP_PROCESS_NAMES[n])) { match = TRUE; break; }
            }
            if (!match) continue;

            if (RegDeleteValueA(hKey, name) == ERROR_SUCCESS) {
                AppendLog(hwnd, "[Startup] removed '%s' (%s) -> %s\r\n",
                          name, roots[r].sub, data);
                removed++;
                i--; subCount--;      /* 删除后后续项索引前移 */
            }
        }
        RegCloseKey(hKey);
    }

    if (removed == 0) {
        AppendLog(hwnd, "[Startup] no classroom autostart entries found\r\n");
    }
    return (removed > 0);
}

/* ------------------------------------------------------------------
 * core.conf 真实格式 (实测): 每行一个 "/键/值/", 值可为空, 行尾为 CRLF。
 * 需要清零的控制类键:
 *   LockedAfterNetBroken - 断网后锁定 (心跳超时 -> 锁屏/锁键)
 *   LockSeat             - 锁定座位 (最难缠, 锁键盘鼠标)
 *   Limit                - 学生端限制总开关
 * ------------------------------------------------------------------ */
static const char* CONFIG_KEYS_TO_ZERO[] = {
    "LockedAfterNetBroken",
    "LockSeat",
    "Limit",
    NULL
};

/* ========== 状态检查函数 ========== */

/* ================= 注入 DLL 版本自检 =================
 *
 * 问题：Student.exe 里已经加载了 bypass_hook32.dll 时，重启 bypass.exe
 * 不会替换它（注入器看到 "已经挂着 hook" 就跳过）。于是"新加的守卫/日志
 * 有没有生效"完全靠猜。
 *
 * 判定方式：直接比较 **磁盘上的 DLL** 和 **目标进程内存里的 DLL** 的
 * PE TimeDateStamp（FileHeader.TimeDateStamp）。这比读日志可靠：
 *   - 两边是同一个编译器产物时，__TIME__ 天然差几秒，不能直接字符串比；
 *   - bypass_hook.log 每次启动都会截断，旧进程的 [build] 行会被抹掉；
 *   - PE 头不受日志、共享内存、版本号影响，而且能一次看到每个进程。
 * 日志里仍然保留一行 [build]（image + built），方便人工 grep。
 */
typedef struct {
    const char* image;
    DWORD       pid;
    DWORD       stamp;      /* 0 = 进程没跑 / DLL 没注入 / 读不到 */
} DLLPROBE;

static DWORD FindPidByName(const char* name)
{
    HANDLE        snap;
    PROCESSENTRY32 pe;
    DWORD         pid = 0;

    if (!name) return 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (!_stricmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

/* PE TimeDateStamp of a file on disk (0 = unreadable). */
static DWORD PeStampFile(const char* path)
{
    HANDLE            f;
    DWORD             rd = 0, stamp = 0;
    IMAGE_DOS_HEADER  dos;
    IMAGE_NT_HEADERS32 nt;

    if (!path || !path[0]) return 0;
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    if (ReadFile(f, &dos, sizeof(dos), &rd, NULL) && rd == sizeof(dos) &&
        dos.e_magic == IMAGE_DOS_SIGNATURE &&
        SetFilePointer(f, dos.e_lfanew, NULL, FILE_BEGIN) != INVALID_SET_FILE_POINTER &&
        ReadFile(f, &nt, sizeof(nt), &rd, NULL) && rd == sizeof(nt) &&
        nt.Signature == IMAGE_NT_SIGNATURE)
        stamp = nt.FileHeader.TimeDateStamp;
    CloseHandle(f);
    return stamp;
}

/* PE TimeDateStamp of the module mapped inside `pid` (0 = not loaded/no access). */
static DWORD PeStampLoaded(DWORD pid, const char* dllName, BOOL matchCache)
{
    HANDLE            snap, hp;
    MODULEENTRY32     me;
    DWORD             stamp = 0;
    SIZE_T            rd = 0;
    IMAGE_DOS_HEADER  dos;
    IMAGE_NT_HEADERS32 nt;

    if (!pid || !dllName) return 0;
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            /* P2 起 Core 从 core.<pid>.<seq>.dll 的缓存副本映射, 名字不再等于
               bypass_hook_core.dll —— 按前缀认。 */
            if (matchCache) {
                if (_strnicmp(me.szModule, "core.", 5) != 0) continue;
            } else if (_stricmp(me.szModule, dllName) != 0) {
                continue;
            }

            hp = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
                             FALSE, pid);
            if (hp) {
                if (ReadProcessMemory(hp, me.modBaseAddr, &dos, sizeof(dos), &rd) &&
                    dos.e_magic == IMAGE_DOS_SIGNATURE &&
                    ReadProcessMemory(hp, (BYTE*)me.modBaseAddr + dos.e_lfanew,
                                      &nt, sizeof(nt), &rd) &&
                    nt.Signature == IMAGE_NT_SIGNATURE)
                    stamp = nt.FileHeader.TimeDateStamp;
                CloseHandle(hp);
            }
            break;
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return stamp;
}

static void StampText(DWORD stamp, char* out, DWORD n)
{
    ULARGE_INTEGER u;
    FILETIME       ft;
    SYSTEMTIME     st;

    if (!out || n < 8) return;
    if (!stamp) {
        _snprintf(out, n - 1, "(unknown)");
        out[n - 1] = '\0';
        return;
    }
    u.QuadPart = ((ULONGLONG)stamp + 11644473600ULL) * 10000000ULL;
    ft.dwLowDateTime  = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    if (FileTimeToLocalFileTime(&ft, &ft) && FileTimeToSystemTime(&ft, &st))
        _snprintf(out, n - 1, "%04u-%02u-%02u %02u:%02u:%02u",
                  (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
                  (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);
    else
        _snprintf(out, n - 1, "stamp:%lu", (unsigned long)stamp);
    out[n - 1] = '\0';
}

/* verbose=1: 把逐进程结果写进日志（给「检查当前状态」用）
   verbose=0: 只在发现旧 DLL 时警告（给启动后的一次性定时器用）
   P1 起有两层: bypass_hook32.dll (Loader, 很少变) 和 bypass_hook_core.dll
   (Core, 改逻辑只换它)。两个都要比, 否则 Core 旧了也报"current"。 */
static BOOL BuildStatusReport(HWND hwnd, BOOL verbose)
{
    static const char* images[] = { "Student.exe", "MultiClient.exe",
                                    "ScreenRecord.exe", "ScreenRender.exe" };
    static const char* dllNames[2] = { "bypass_hook32.dll",
                                       "bypass_hook_core.dll" };
    static const char* dllTags[2]  = { "loader", "core" };
    char  dir[MAX_PATH] = "";
    char  stale[200] = "";
    DWORD disk[2] = { 0, 0 };
    int   i, d, staleCount = 0;

    if (GetModuleFileNameA(NULL, dir, sizeof(dir))) {
        char* p = strrchr(dir, '\\');
        if (p) *(p + 1) = '\0';
        else dir[0] = '\0';
    }

    for (d = 0; d < 2; d++) {
        char path[MAX_PATH] = "";

        if (dir[0]) PathCombineA(path, dir, dllNames[d]);
        disk[d] = path[0] ? PeStampFile(path) : 0;
    }

    if (verbose) {
        char t1[40], t2[40];

        AppendLog(hwnd, "\r\n--- Guard Status ---\r\n");
        AppendLog(hwnd, "  screen guard   : %s (bin\\bypass_screen.ini)\r\n",
                  g_screenGuard ? "ON" : "off");
        AppendLog(hwnd, "  shutdown guard : %s (bin\\bypass_shut.ini)\r\n",
                  g_shutGuard ? "ON" : "off");
        StampText(disk[0], t1, sizeof(t1));
        StampText(disk[1], t2, sizeof(t2));
        AppendLog(hwnd, "  DLL on disk    : loader %s / core %s\r\n",
                  disk[0] ? t1 : "NOT FOUND", disk[1] ? t2 : "NOT FOUND");
    }

    for (i = 0; i < (int)(sizeof(images) / sizeof(images[0])); i++) {
        DWORD pid = FindPidByName(images[i]);
        char  line[256] = "";
        int   bad = 0;

        if (!pid) {
            if (verbose)
                AppendLog(hwnd, "  %-14s : not running\r\n", images[i]);
            continue;
        }

        for (d = 0; d < 2; d++) {
            DWORD stamp = PeStampLoaded(pid, dllNames[d], d == 1);
            char  t[40];
            char  part[96];

            if (!stamp) {
                _snprintf(part, sizeof(part) - 1, "%s not loaded", dllTags[d]);
            } else {
                StampText(stamp, t, sizeof(t));
                if (disk[d] && stamp != disk[d]) {
                    _snprintf(part, sizeof(part) - 1, "%s %s <== OLD", dllTags[d], t);
                    bad = 1;
                    if (stale[0]) strncat(stale, ", ", sizeof(stale) - strlen(stale) - 1);
                    {
                        char one[64];
                        _snprintf(one, sizeof(one) - 1, "%s(%s)", images[i], dllTags[d]);
                        one[sizeof(one) - 1] = '\0';
                        strncat(stale, one, sizeof(stale) - strlen(stale) - 1);
                    }
                    staleCount++;
                } else {
                    _snprintf(part, sizeof(part) - 1, "%s %s", dllTags[d], t);
                }
            }
            part[sizeof(part) - 1] = '\0';
            if (line[0]) strncat(line, " / ", sizeof(line) - strlen(line) - 1);
            strncat(line, part, sizeof(line) - strlen(line) - 1);
        }

        if (verbose)
            AppendLog(hwnd, "  %-14s : %s%s\r\n", images[i], line,
                      bad ? "   (restart the classroom client)" : "");
    }

    if (staleCount) {
        AppendLog(hwnd, "\r\n[Build] WARNING: %s 里跑的是旧 DLL。\r\n"
                        "[Build] 旧守卫仍然有效，但新逻辑/日志要重启课堂客户端才会加载。\r\n",
                  stale);
        UpdateStatus(hwnd, "Status: WARNING - old DLL in classroom client (restart it)");
        return TRUE;
    }

    if (verbose)
        AppendLog(hwnd, "  (all classroom processes run the current DLLs)\r\n");
    return FALSE;
}

BOOL CheckStatus(HWND hwnd) {
    AppendLog(hwnd, "\r\n[Status] Checking system status...\r\n");
    UpdateStatus(hwnd, "Status: Checking...");
    
    /* 检查进程 */
    AppendLog(hwnd, "\r\n--- Process Status ---\r\n");
    for (int i = 0; TARGET_PROCESSES[i]; i++) {
        BOOL running = IsProcessRunning(TARGET_PROCESSES[i]);
        AppendLog(hwnd, "  %-20s: %s\r\n", TARGET_PROCESSES[i], 
            running ? "RUNNING" : "NOT RUNNING");
    }
    
    /* 检查驱动 */
    AppendLog(hwnd, "\r\n--- Driver Status ---\r\n");
    BOOL driverInstalled = IsDriverInstalled();
    AppendLog(hwnd, "  %-20s: %s\r\n", DRIVER_SERVICE_NAME,
        driverInstalled ? "INSTALLED" : "NOT INSTALLED");

    /* 注入 DLL 的版本自检 + 两个守卫的开关状态 (磁盘 DLL vs 进程内 DLL) */
    BuildStatusReport(hwnd, TRUE);
    
    /* 检查配置 (用安装目录定位, 而不是 exe 所在目录; 不再用硬编码偏移取值) */
    AppendLog(hwnd, "\r\n--- Config Status ---\r\n");
    char configPath[MAX_PATH] = {0};
    if (!FindSoftwareFile("skin\\core.conf", configPath, sizeof(configPath))) {
        AppendLog(hwnd, "  Config file: NOT FOUND\r\n");
    } else {
        AppendLog(hwnd, "  Config file: %s\r\n", configPath);
        for (int k = 0; CONFIG_KEYS_TO_ZERO[k]; k++) {
            char value[64] = {0};
            if (ReadConfigValue(configPath, CONFIG_KEYS_TO_ZERO[k], value, sizeof(value))) {
                AppendLog(hwnd, "    %-22s = %s\r\n", CONFIG_KEYS_TO_ZERO[k], value);
            } else {
                AppendLog(hwnd, "    %-22s = (absent)\r\n", CONFIG_KEYS_TO_ZERO[k]);
            }
        }
    }

    /* 实时锁定状态 (实证项) */
    AppendLog(hwnd, "\r\n--- Runtime Lock State ---\r\n");
    {
        char cur[128] = "";

        if (GetInputDesktopName(cur, sizeof(cur))) {
            AppendLog(hwnd, "  input desktop   : %s%s\r\n", cur,
                      IsLockDesktopActive() ? "   <== LOCKED (MCLOCK Desktop)" : "");
        } else {
            /* OpenInputDesktop 失败 = UAC 安全桌面 / 锁屏 / 桌面被切走。
               这时"锁没锁"是**未知**, 不能当成"没锁" (以前这里只打 (unknown),
               很容易被读成"没锁")。 */
            AppendLog(hwnd, "  input desktop   : (cannot query: UAC secure desktop / "
                            "locked / switched away - lock state UNKNOWN)\r\n");
        }
        AppendLog(hwnd, "  unlock IOCTL    : 0x%X (%s)\r\n", IOCTL_KEYBOARD_ALL_RELEASE,
                  (g_workingIoctl == IOCTL_KEYBOARD_ALL_RELEASE) ? "verified" : "not verified yet");
    }

    /* 只读项: 之前注释里提到"含 View 实时值"但代码没读, 这里补上 */
    if (configPath[0]) {
        char view[64] = {0};
        if (ReadConfigValue(configPath, "View", view, sizeof(view)))
            AppendLog(hwnd, "  View (read-only): %s\r\n", view);
    }

    UpdateStatus(hwnd, "Status: Check completed");
    return TRUE;
}

/* ========== 一键执行函数 ========== */

/* 一键绕过核心 (不含弹窗, 可被线程调用) */
static BOOL AutoAllCore(HWND hwnd) {
    AppendLog(hwnd, "\r\n========================================\r\n");
    AppendLog(hwnd, "  AUTO BYPASS: Starting...\r\n");
    AppendLog(hwnd, "========================================\r\n");

    UpdateStatus(hwnd, "Status: Auto bypass in progress...");

    /* 0. 先拿到特权 (SeDebugPrivilege 等), 否则后面杀进程会大面积 ACCESS_DENIED */
    EnableRequiredPrivileges();

    /* 1. 清自启动项 (实证: Startup.exe 写 Run 键; 不清会被重新拉起) */
    RemoveStartupPersistence(hwnd);
    Sleep(200);

    /* 2. 终止进程 (TARGET_PROCESSES 已按"先看守后主体"排序) */
    KillControlProcesses(hwnd);
    Sleep(500);

    /* 3. 卸载驱动 (含键盘类 UpperFilters 清理) */
    UninstallDriver(hwnd);
    Sleep(500);

    /* 4. 广播窗口小窗化 (静默) —— 先预设 T1.5 禁用全屏优化, 再做窗口层降级 */
    PresetFullscreenDowngrade(hwnd, TRUE);
    SmallWindowBroadcastCore(hwnd, TRUE);

    /* 5. 收尾: 再看一眼是否被看守进程拉回来 */
    Sleep(300);
    if (IsProcessRunning("Student.exe") || IsProcessRunning("BlackSlient.exe")) {
        AppendLog(hwnd, "[Auto] watchdog relaunched something, re-killing...\r\n");
        KillControlProcesses(hwnd);
    }

    AppendLog(hwnd, "\r\n========================================\r\n");
    AppendLog(hwnd, "  AUTO BYPASS: Done!\r\n");
    AppendLog(hwnd, "========================================\r\n");

    UpdateStatus(hwnd, "Status: Bypass completed!");
    return TRUE;
}

/* /auto 的后台执行体。
   ★ AutoAllCore 里有 Sleep×4 + 等驱动停止 (每个最长 3 秒) + 等卸载器 (10 秒),
     放在 UI 线程会"未响应"。这里把它整个挪到工作线程, 日志照常流回编辑框。 */
static volatile LONG g_autoAllBusy = 0;

static DWORD WINAPI AutoAllThread(LPVOID lp)
{
    AutoAllCore((HWND)lp);
    InterlockedExchange(&g_autoAllBusy, 0);
    return 0;
}

/* ============================================================================
 *  广播窗口小窗化
 *
 *  三层结构 (越往下越硬, 每层都回读验证):
 *    Tier 1.5  预设"禁用全屏优化"      只对"独占全屏/全屏优化"有效, 下次启动生效
 *    Tier 1    窗口层降级 (基础+加固) 去置顶/去POPUP/去最大化/沉底/缩放
 *    轮询兜底  事件钩子之外的窗口      1.2s 周期
 *
 *  核心: DemoteWindowTiered() 会自己升级并在结束时回读验证;
 *  返回 0 = 窗口层彻底失败 (疑似独占全屏), 只能提示重启渲染进程。
 * ========================================================================== */

/* 广播/投屏相关进程 */
static const char* BROADCAST_PROCESSES[] = {
    "ScreenRender.exe", "MultiRender.exe", "MultiClient.exe",
    "AudioRender.exe", "AudioSender.exe",
    "Student.exe", "SharedDesktop.exe", "StudentScreenHelper.exe",
    "ScreenSender.exe", NULL
};

/* 已知广播窗口类名 (SDL 渲染窗 / MultiClient 渲染窗)
   注: 反编译资料里出现的 MultiRenderWindowClass 一并列入, 避免漏窗口。 */
/* pid -> 可执行文件名 (不带路径) */
static BOOL GetPidImageName(DWORD pid, char* out, DWORD size)
{
    char  full[MAX_PATH] = {0};
    char* p;

    if (!out || size == 0) return FALSE;
    out[0] = '\0';
    if (!GetProcessPath(pid, full, sizeof(full))) return FALSE;
    p = strrchr(full, '\\');
    snprintf(out, size, "%s", p ? p + 1 : full);
    out[size - 1] = '\0';
    return TRUE;
}

/* 渲染器窗口类白名单。
   ★ 为什么必须按类名收口: Student.exe / MultiClient.exe 里有自己的界面窗口
     (实测 StudentHelperWnd.oseasy —— 学生的浮动工具条/虚拟桌面面板, 平时
     只有 350x70), 它们和广播窗口在同一个进程里。只按 "进程命中 + 尺寸够大"
     判断的话, 这个面板一旦临时铺满屏 (或者被强制模式扫到) 就会被我们套上
     小窗, 然后 "keep user geometry" 又把它钉死, 用户看到的就是
     "软件自己把虚拟桌面包成了小窗"。 */
static BOOL IsRenderWindowClass(const char* cls)
{
    if (!cls || !cls[0]) return FALSE;
    if (_strnicmp(cls, "MultiRender", 11) == 0) return TRUE;          /* MultiRenderWindowClass / .h_ckgb */
    if (_strnicmp(cls, "StreamRenderWindow", 18) == 0) return TRUE;
    if (_strnicmp(cls, "OeScreenRenderWindow", 20) == 0) return TRUE;
    if (_stricmp(cls, "SDL_app") == 0) return TRUE;
    return FALSE;
}

/* 进程本身是不是"只负责画广播画面"的渲染器 */
static BOOL IsRendererImage(const char* img)
{
    if (!img || !img[0]) return FALSE;
    return _stricmp(img, "ScreenRender.exe") == 0 ||
           _stricmp(img, "MultiRender.exe") == 0 ||
           _stricmp(img, "SharedDesktop.exe") == 0;
}

static const char* BROADCAST_CLASSES[] = {
    "SDL_app",
    "StreamRenderWindow.h_ckgb",
    "MultiRenderWindow.h_ckgb",
    "MultiRenderWindowClass",
    "OeScreenRenderWindow",
    NULL
};

/* --- 事件驱动即时小窗化 (SetWinEventHook) 的全局状态 --- */
#define IDT_DEMOTE_RECHECK   0x51     /* 广播窗口出现后的 150ms 复核定时器 */
#define MAX_PENDING_DEMOTE   16

static HWINEVENTHOOK g_hBroadcastHook = NULL;   /* 全局 WinEvent 钩子 */
static HWND   g_pendingDemote[MAX_PENDING_DEMOTE];
static int    g_pendingDemoteCount = 0;
/* 缓存的广播/投屏进程 PID, 供事件回调做廉价判断 (避免每次事件都枚举进程) */
static DWORD  g_bcastPids[32];
static int    g_bcastPidCount = 0;
static DWORD  g_bcastPidTick  = 0;

typedef struct {
    DWORD pids[32];
    int   npid;
    int   screenW, screenH;
    int   demoted;
    int   fullscreen;      /* 本轮看到的"满屏"窗口数 (含降级失败) → 决定复核节拍 */
    int   small;           /* 本轮看到的"已接管的小窗"数 → 触发自动解锁键盘 */
    HWND  firstHwnd;       /* 本轮第一个被降级的窗口, 用于判断"改回去"还是"重建窗口" */
    int   lastW, lastH;    /* 降级后回读到的实际尺寸 (验证"到底变小没有") */
    BOOL  quiet;
    HWND  logWnd;
} DemoteCtx;

static BOOL CollectBroadcastPids(DemoteCtx* ctx) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 pe;
    int i;

    if (snap == INVALID_HANDLE_VALUE) return FALSE;
    pe.dwSize = sizeof(pe);
    ctx->npid = 0;

    if (Process32First(snap, &pe)) {
        do {
            for (i = 0; BROADCAST_PROCESSES[i]; i++) {
                if (_stricmp(pe.szExeFile, BROADCAST_PROCESSES[i]) == 0 &&
                    ctx->npid < 32) {
                    ctx->pids[ctx->npid++] = pe.th32ProcessID;
                    break;
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return (ctx->npid > 0);
}

/* 刷新广播进程 PID 缓存 (2 秒内复用, 避免事件风暴时反复枚举进程) */
static void RefreshBroadcastPids(void) {
    DWORD now = GetTickCount();
    DemoteCtx ctx;

    if (g_bcastPidCount > 0 && (now - g_bcastPidTick) < 2000) return;

    memset(&ctx, 0, sizeof(ctx));
    if (CollectBroadcastPids(&ctx)) {
        memcpy(g_bcastPids, ctx.pids, sizeof(g_bcastPids));
        g_bcastPidCount = ctx.npid;
    } else {
        g_bcastPidCount = 0;
    }
    g_bcastPidTick = now;
}

static BOOL IsBroadcastPid(DWORD pid) {
    for (int i = 0; i < g_bcastPidCount; i++) {
        if (g_bcastPids[i] == pid) return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------
 *  满屏判定 + 私有桌面复位  (反制核心)
 *
 *  REA 实证:
 *    MultiRender.exe  FUN_0040a790 / FUN_0040ac10:
 *        SetWindowPos(hwnd, HWND_TOPMOST (即 -1), 0, 0, cx, cy, 0)
 *        SetWindowLongW(GWL_STYLE, style & 0xFF3BFFFF | WS_POPUP)
 *        ShowCursor(0)
 *    ScreenRender.exe FUN_00411160:
 *        500ms WM_TIMER 里再补一次 SetWindowPos(HWND_TOPMOST)
 *    MultiClient.exe  FUN_004032d0:
 *        OpenDesktopA/CreateDesktopA("NetTeachingDesktop") + SwitchDesktop
 *    MultiClient.exe  FUN_004028a0:
 *        SetWindowsHookExW(WH_MOUSE_LL) 只放行 dwExtraInfo==0x12452 的鼠标事件
 *
 *  由此得到两条必须遵守的规则:
 *   1) "满屏"要按几何尺寸判断, 不能只靠窗口类名 (类名会变, 尺寸不会);
 *   2) 一旦被切到 NetTeachingDesktop, 本进程所属桌面上的 EnumWindows
 *      根本枚举不到广播窗口 —— 必须先切回用户桌面再扫。
 * ------------------------------------------------------------------ */

/* 满屏判定: 必须真的"铺满整屏" (宽高都 >= 屏幕 90%)。
   ⚠ 不要再加"置顶 + 2/3 屏也算满屏"的宽松分支 —— 实测 (14:12 日志)
   教室软件的「虚拟桌面」预览窗就是 1344x637 @ 1792x850 (正好 75%) + 置顶
   的普通带框窗口, 宽松判定会把它也当成广播窗降级, 屏幕上于是出现两个小窗。
   虚拟桌面窗用户本来就能自己拖, 完全不需要我们插手。 */
static BOOL IsWindowFullscreen(HWND hwnd, int screenW, int screenH) {
    RECT r;
    int  w, h;

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return FALSE;
    if (!GetWindowRect(hwnd, &r)) return FALSE;

    w = r.right - r.left;
    h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return FALSE;

    return (w >= screenW * 9 / 10 && h >= screenH * 9 / 10);
}

/* 已经是个"普通窗口" = 有标题栏, 而且没有铺满屏。
   这类窗口 (虚拟桌面预览、我们已经降级过的小窗、用户自己拖出来的窗口)
   一律不再碰 —— 它们本来就是可拖可缩的。 */
static BOOL IsPlainWindow(HWND hwnd, int screenW, int screenH) {
    LONG_PTR st;
    RECT     r;

    if (!IsWindow(hwnd)) return FALSE;
    st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (!(st & WS_CAPTION)) return FALSE;
    if (!GetWindowRect(hwnd, &r)) return TRUE;

    return !((r.right - r.left) >= screenW * 9 / 10 &&
             (r.bottom - r.top) >= screenH * 9 / 10);
}

/* 若输入桌面不是 Default, 切回 Default。
   实证: 投屏会话会新建 "NetTeachingDesktop" 并 SwitchDesktop 过去。
   ⚠ 只允许在"用户主动点击"的路径调用 (持续保持小窗)。
   轮询线程里绝不能调: 那会与 MultiClient 的 SwitchDesktop 互相抢, 每次切换
   都是整屏重绘, 用户看到的就是"黑屏闪动"。
   返回 TRUE = 确实执行了切换。 */
/* ============================================================================
 *  Tier 1.5: 预设"禁用全屏优化"兼容性标志
 *
 *  教师端"全屏播放"如果是独占全屏 (DXGI) 或无边框全屏优化, 任何窗口层操作
 *  都救不回来 (SetWindowPos 会被立刻还原)。此时唯一干净的办法是让这个进程
 *  不要用独占全屏: 写 AppCompatFlags\Layers 的 DISABLEDXMAXIMIZEDWINDOWEDMODE
 *  (就是 exe 属性->兼容性->"禁用全屏优化" 勾选后写的那一项)。
 *  标志在该进程**下次启动**时生效, 所以这里只是"提前布防";
 *  之后 Tier 1 才能生效。
 * ========================================================================== */
#define FULLSCREEN_DOWNGRADE_FLAG   L"~ DISABLEDXMAXIMIZEDWINDOWEDMODE"
#define APPCOMPAT_LAYERS_KEY        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\AppCompatFlags\\Layers"

/* 给单个 exe 写"禁用全屏优化"; 返回 TRUE = 已就位 (新写或本来就有) */
static BOOL SetFullscreenDowngradeFor(const char* exePath, HWND hwndLog, BOOL quiet) {
    HKEY    hKey = NULL;
    wchar_t wPath[MAX_PATH * 2];
    DWORD   cb;
    BOOL    applied = FALSE;

    if (!exePath || !exePath[0]) return FALSE;
    if (!MultiByteToWideChar(CP_ACP, 0, exePath, -1, wPath, MAX_PATH * 2)) return FALSE;

    if (RegCreateKeyExW(HKEY_CURRENT_USER, APPCOMPAT_LAYERS_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_QUERY_VALUE | KEY_SET_VALUE,
                        NULL, &hKey, NULL) != ERROR_SUCCESS) {
        return FALSE;
    }

    /* 1) 读现有值: 先查**真实大小**再读。RegQueryValueExW 在缓冲区不足时返回
       ERROR_MORE_DATA 且只写半截 —— 以前固定 256 wchar, 长值会走"追加"分支,
       用空串把用户原有的 RUNASADMIN / HIGHDPIAWARE 等兼容标志覆盖掉。 */
    {
        DWORD    type = 0, size = 0;
        wchar_t* cur = NULL;

        if (RegQueryValueExW(hKey, wPath, NULL, &type, NULL, &size) == ERROR_SUCCESS &&
            size >= sizeof(wchar_t) && size <= 64 * 1024 &&
            (type == REG_SZ || type == REG_EXPAND_SZ) &&
            (cur = (wchar_t*)malloc(size + sizeof(wchar_t))) != NULL) {
            DWORD got = size;

            memset(cur, 0, size + sizeof(wchar_t));
            if (RegQueryValueExW(hKey, wPath, NULL, &type, (LPBYTE)cur, &got) == ERROR_SUCCESS) {
                cur[size / sizeof(wchar_t)] = L'\0';

                if (wcsstr(cur, L"DISABLEDXMAXIMIZEDWINDOWEDMODE")) {
                    free(cur);
                    RegCloseKey(hKey);
                    return TRUE;
                }

                /* 2) 追加标志, 不覆盖原有内容 */
                {
                    size_t  need = wcslen(cur) + wcslen(FULLSCREEN_DOWNGRADE_FLAG) + 3;
                    wchar_t* merged = (wchar_t*)malloc(need * sizeof(wchar_t));

                    if (merged) {
                        size_t         n = wcslen(cur);
                        const wchar_t* sep = (n && cur[n - 1] != L' ') ? L" " : L"";

                        _snwprintf(merged, need - 1, L"%s%s%s", cur, sep,
                                   FULLSCREEN_DOWNGRADE_FLAG);
                        merged[need - 1] = L'\0';
                        if (RegSetValueExW(hKey, wPath, 0, REG_SZ, (const BYTE*)merged,
                                           (DWORD)((wcslen(merged) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS)
                            applied = TRUE;
                        free(merged);
                    }
                }
            }
            free(cur);
        }
    }

    /* 3) 原来没有值 (或读不出来): 直接写标志 */
    if (!applied) {
        cb = (DWORD)((wcslen(FULLSCREEN_DOWNGRADE_FLAG) + 1) * sizeof(wchar_t));
        if (RegSetValueExW(hKey, wPath, 0, REG_SZ,
                           (const BYTE*)FULLSCREEN_DOWNGRADE_FLAG, cb) == ERROR_SUCCESS)
            applied = TRUE;
    }

    if (applied) {
        if (!quiet && hwndLog)
            AppendLog(hwndLog,
                "[SmallWin][T1.5] disable fullscreen-optimization preset: %s "
                "(effective next start)\r\n", exePath);
    } else if (!quiet && hwndLog) {
        AppendLog(hwndLog,
            "[SmallWin][T1.5] preset failed for %s (err=%lu)\r\n",
            exePath, GetLastError());
    }

    RegCloseKey(hKey);
    return applied;
}

/* 为所有已知广播进程预设"禁用全屏优化"; 返回已就位数量 */
static int PresetFullscreenDowngrade(HWND hwndLog, BOOL quiet) {
    HANDLE         snap;
    PROCESSENTRY32 pe;
    char           path[MAX_PATH];
    int            done = 0, i;

    /* 1) 运行中的广播进程: 用真实路径 (最准确) */
    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        pe.dwSize = sizeof(pe);
        if (Process32First(snap, &pe)) {
            do {
                for (i = 0; BROADCAST_PROCESSES[i]; i++) {
                    if (_stricmp(pe.szExeFile, BROADCAST_PROCESSES[i]) == 0) {
                        if (GetProcessPath(pe.th32ProcessID, path, sizeof(path)) &&
                            SetFullscreenDowngradeFor(path, hwndLog, quiet)) {
                            done++;
                        }
                        break;
                    }
                }
            } while (Process32Next(snap, &pe));
        }
        CloseHandle(snap);
    }

    /* 2) 没运行也提前布防: 用安装目录里的路径 */
    for (i = 0; BROADCAST_PROCESSES[i]; i++) {
        if (FindSoftwareFile(BROADCAST_PROCESSES[i], path, sizeof(path)) &&
            SetFullscreenDowngradeFor(path, hwndLog, quiet)) {
            done++;
        }
    }

    if (!quiet && hwndLog)
        AppendLog(hwndLog,
            "[SmallWin][T1.5] fullscreen-downgrade preset done (%d exe; "
            "takes effect after those processes restart)\r\n", done);
    return done;
}

/* ============================================================================
 *  Tier 1: 窗口层降级 (基础 -> 加固), 每次操作后回读验证
 * ========================================================================== */

/* Win11: SetWindowBand 可以把窗口压到桌面最底层 band, 比 HWND_NOTOPMOST 更硬。
   动态加载; 旧系统拿不到就自动跳过 (不失败)。 */
#ifndef ZBID_DESKTOP
#define ZBID_DESKTOP 1
#endif
typedef BOOL (WINAPI *PFN_SetWindowBand)(HWND, HWND, DWORD);
static PFN_SetWindowBand g_pfnSetWindowBand = NULL;

static void InitWindowBandApi(void) {
    HMODULE hU;
    if (g_pfnSetWindowBand) return;
    hU = GetModuleHandleW(L"user32.dll");
    if (hU) {
        g_pfnSetWindowBand = (PFN_SetWindowBand)(void*)
            GetProcAddress(hU, "SetWindowBand");
    }
}

/* 小窗外框样式: 与实际降级后套上的样式一致, 用于把"客户区尺寸"换成
   "含标题栏/边框的外框尺寸" (AdjustWindowRectEx) */
#define SMALL_FRAME_STYLE (WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | \
                           WS_MINIMIZEBOX | WS_MAXIMIZEBOX)

/* 右下角贴边摆放指定外框尺寸的小窗 */
static void PlaceSmallRect(int w, int h, int screenW, int screenH, RECT* out) {
    RECT wa;

    wa.left = 0; wa.top = 0; wa.right = screenW; wa.bottom = screenH;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);

    out->left = wa.right - w - 24;
    out->top  = wa.bottom - h - 24;
    if (out->left < 0) out->left = 0;
    if (out->top  < 0) out->top  = 0;
    out->right  = out->left + w;
    out->bottom = out->top + h;
}

/* 按"源窗口"的宽高比算小窗矩形。
   ★ 视频是铺满客户区的: 源窗口是 4:3 / 16:10 时硬塞进 16:9 的小窗,
     画面就会被拉扁。旧实现写死 h = w*9/16, 在 16:10 屏幕上必然变形。
   srcW/srcH = 降级前的源窗口尺寸; 传 0 表示未知 (= 按屏幕比例)。 */
static void ComputeSmallRectEx(int srcW, int srcH, int screenW, int screenH, RECT* out) {
    RECT rc;
    int  w, h;

    if (srcW < 16 || srcH < 16) { srcW = screenW; srcH = screenH; }
    if (srcW < 16) srcW = 16;
    if (srcH < 16) srcH = 16;

    /* 客户区宽度 = 屏宽/3, 高度按源比例推导; 太高就压到半屏高再反推宽度 */
    w = screenW / 3;
    if (w < 320) w = 320;
    h = srcH * w / srcW;
    if (h > screenH / 2) {
        h = screenH / 2;
        w = srcW * h / srcH;
    }
    if (w < 160) { w = 160; h = srcH * w / srcW; }
    if (h < 90)  h = 90;

    /* 客户区 -> 外框 (标题栏 + 可缩放边框) */
    rc.left = rc.top = 0;
    rc.right = w;
    rc.bottom = h;
    if (!AdjustWindowRectEx(&rc, SMALL_FRAME_STYLE, FALSE, 0)) {
        rc.left = rc.top = 0;
        rc.right = w;
        rc.bottom = h;
    }
    PlaceSmallRect(rc.right - rc.left, rc.bottom - rc.top, screenW, screenH, out);
}

/* 目标小窗矩形 (源尺寸未知时按屏幕比例; 供"是否已就位"比较用) */
static void ComputeSmallRect(int screenW, int screenH, RECT* out) {
    ComputeSmallRectEx(0, 0, screenW, screenH, out);
}

/* 目标尺寸记在窗口属性上 (跨进程可见)。这样"是否已就位"比较的是同一个数:
   否则源窗口不是满屏时按屏幕比例反推的 target 与实际窗口差几十像素,
   每 200ms 就会重设一次 -> 用户看到的就是闪。 */
#define PROP_WANT_W "BypassWantW"
#define PROP_WANT_H "BypassWantH"

static void SmallTargetFor(HWND hwnd, int screenW, int screenH, RECT* out) {
    RECT src;
    int  tw = (int)(INT_PTR)GetPropA(hwnd, PROP_WANT_W);
    int  th = (int)(INT_PTR)GetPropA(hwnd, PROP_WANT_H);

    if (tw >= 160 && th >= 90) {          /* 之前算过: 沿用同一个目标 */
        PlaceSmallRect(tw, th, screenW, screenH, out);
        return;
    }

    if (GetWindowRect(hwnd, &src))
        ComputeSmallRectEx(src.right - src.left, src.bottom - src.top,
                           screenW, screenH, out);
    else
        ComputeSmallRectEx(0, 0, screenW, screenH, out);

    SetPropA(hwnd, PROP_WANT_W, (HANDLE)(INT_PTR)(out->right - out->left));
    SetPropA(hwnd, PROP_WANT_H, (HANDLE)(INT_PTR)(out->bottom - out->top));
}

/* 窗口是否已经处于"目标小窗形态"。
   ★ 这是消除"每 200ms 闪一下"的关键:
   DemoteEnumProc 只要类名命中就会调用 DemoteWindowTiered, 而
   DemoteWindowBase 每次都做 SetWindowPos(SWP_FRAMECHANGED) + ShowWindow ——
   对渲染窗口来说每次都会触发一次重建/重绘 (hbrBackground = BLACK_BRUSH),
   于是 200ms 一闪。已经在目标形态就绝不能再碰它。 */
static BOOL IsAlreadyDemoted(HWND hwnd, int screenW, int screenH) {
    RECT    r, want;
    LONG_PTR st, ex;

    if (!IsWindow(hwnd) || !IsWindowVisible(hwnd)) return FALSE;
    if (IsZoomed(hwnd) || IsIconic(hwnd)) return FALSE;
    if (!GetWindowRect(hwnd, &r)) return FALSE;

    /* 用户主动(模拟)最大化: 这已经是我们接管的形态, 不要按像素再判一次。
       标题栏自动隐藏时窗口没有 WS_CAPTION, 旧判据会把它当成"未就位",
       每 200ms 重新降级一次 —— 必须在这里放过。 */
    if (GetPropA(hwnd, "BypassUserMax")) return TRUE;

    st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    /* 只要还是置顶 / POPUP / 最大化, 就不算"已就位" */
    if (ex & WS_EX_TOPMOST) return FALSE;
    if (st & (WS_POPUP | WS_MAXIMIZE)) return FALSE;
    if (!(st & WS_CAPTION)) return FALSE;

    /* ★ 已经接管过、而且确实是个小窗 -> 直接算"已就位", 不再纠结像素差。
       以前这里只用 BypassWant ±8 精确比较: 两次算出来的边框高度差 11px
       (见 15:44 日志 "app wanted 1666,766 868x456" vs want 868x467),
       就会每 200ms 重新降级一次 —— 然后被 DLL 的"几何冻结"挡住, 于是
       日志里 keep user geometry 刷屏、窗口也在两套尺寸之间来回抢。
       判据改成"接管过 + 不算大", 与具体像素无关。 */
    if (GetPropA(hwnd, "BypassSmall")) {
        int w = (int)(r.right - r.left);
        int hh = (int)(r.bottom - r.top);
        if (w <= screenW / 2 && hh <= screenH / 2) return TRUE;
    }

    /* 尺寸已经是目标小窗 (±8px 容差)。优先用降级时记下的目标尺寸 */
    {
        int tw = (int)(INT_PTR)GetPropA(hwnd, PROP_WANT_W);
        int th = (int)(INT_PTR)GetPropA(hwnd, PROP_WANT_H);

        if (tw <= 0 || th <= 0) {
            ComputeSmallRect(screenW, screenH, &want);
            tw = want.right - want.left;
            th = want.bottom - want.top;
        }
        if (abs((int)(r.right - r.left) - tw) > 8) return FALSE;
        if (abs((int)(r.bottom - r.top) - th) > 8) return FALSE;
    }

    return TRUE;
}

/* 已经被我们小窗化过的窗口: 窗口属性 + 进程内小表双保险。
   ★ 这是"用户手动拖大/最大化之后又被自动拉回右下角"的根因修复:
   旧逻辑只用 IsAlreadyDemoted() 按几何尺寸判断"是否已就位", 用户一旦
   改了大小就被判成"未就位", 下一轮 200ms 轮询又把窗口按回小窗。
   现在的规则是: 接管过的窗口, 除非它又变回"满屏/置顶",
   否则一律不碰 —— 尺寸和位置完全交给用户。 */
/* ------------------- 已被接管的窗口标记 -------------------
   只认窗口属性 (BypassSmall): 属性挂在窗口对象上, 窗口销毁时系统一起回收,
   不会出现"句柄复用 -> 误判已接管"。
   以前还有一张进程内 HWND 表兜底, 但表项只比数值: 旧广播窗销毁后新窗口
   拿到同一个句柄, IsManagedWin() 直接返回 TRUE -> SHOW 事件里连队都不排,
   新广播窗只能等 200ms 轮询 (而且会被当成"用户拖过的窗口"放过)。 */
static void MarkManagedWin(HWND h) {
    if (!h || !IsWindow(h)) return;
    if (!SetPropA(h, "BypassSmall", (HANDLE)1))
        SafeLog("[SmallWin] WARNING: SetProp(BypassSmall) failed hwnd=%p err=%lu\r\n",
                (void*)h, (unsigned long)GetLastError());
}

static BOOL IsManagedWin(HWND h) {
    if (!h || !IsWindow(h)) return FALSE;
    return GetPropA(h, "BypassSmall") ? TRUE : FALSE;
}

/* 基础降级: 去置顶 + 普通窗口样式 + 缩成右下角小窗 (原实现) */
static BOOL DemoteWindowBase(HWND hwnd, int screenW, int screenH) {
    LONG_PTR style, ex;
    RECT    rc;

    if (!IsWindow(hwnd)) return FALSE;
    /* ★ 最小化到任务栏的窗口绝不能在这里"救"回来, 也不该按小窗重排:
       跨进程 ShowWindow/SetWindowPos 不经过被注入 DLL 的钩子, 用户会看到
       任务栏里的窗口自己弹出来。加固档 (DemoteWindowHard) 一直有这条保护,
       基础档漏了 —— 最小化窗口的 GetWindowRect 还会让验证误判"成功"。 */
    if (IsIconic(hwnd)) return FALSE;

    /* ★ 用户主动(模拟)最大化过的窗口: 绝不再降级。
       标记由注入的 DLL 打在窗口属性上 (跨进程可读)。 */
    if (GetPropA(hwnd, "BypassUserMax")) return FALSE;

    /* 先量源窗口 (改样式前后外框尺寸会变), 按它的宽高比定目标 */
    SmallTargetFor(hwnd, screenW, screenH, &rc);

    /* 1) 取消置顶 */
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

    /* 2) 变成可拖动/可缩放的普通窗口 (标题栏+边框+系统菜单) */
    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    style &= ~(LONG_PTR)WS_POPUP;
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    /* 3) 去掉工具窗口扩展样式, 让它出现在任务栏 */
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~(LONG_PTR)WS_EX_TOOLWINDOW;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    /* 4) 应用新样式帧 + 新位置 */
    SetWindowPos(hwnd, HWND_NOTOPMOST, rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return TRUE;
}

/* 加固降级: 针对"渲染器反复改回全屏/最大化"的窗口 ——
   强制退出最大化 + 去 POPUP + 沉到最底 + (Win11) 打到最底 band。 */
static BOOL DemoteWindowHard(HWND hwnd, int screenW, int screenH) {
    LONG_PTR style, ex;
    RECT    rc;

    if (!IsWindow(hwnd)) return FALSE;
    if (GetPropA(hwnd, "BypassUserMax")) return FALSE;
    /* ★ 用户已经最小化到任务栏 -> 绝不能在这里"救"回来:
       跨进程 ShowWindow(SW_RESTORE) 不经过被注入 DLL 的任何钩子。 */
    if (IsIconic(hwnd)) return FALSE;

    /* 先定目标 (最大化状态下量到的就是满屏矩形, 比例是对的) */
    SmallTargetFor(hwnd, screenW, screenH, &rc);

    if (IsZoomed(hwnd))
        ShowWindow(hwnd, SW_RESTORE);

    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    style |= WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU;
    style &= ~(LONG_PTR)(WS_POPUP | WS_MAXIMIZE);
    SetWindowLongPtrW(hwnd, GWL_STYLE, style);

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    ex &= ~(LONG_PTR)(WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex);

    /* 顺序: 先去置顶 -> 沉底 -> 摆位置 */
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(hwnd, HWND_BOTTOM, rc.left, rc.top,
                 rc.right - rc.left, rc.bottom - rc.top,
                 SWP_FRAMECHANGED | SWP_NOACTIVATE);

    InitWindowBandApi();
    if (g_pfnSetWindowBand)
        g_pfnSetWindowBand(hwnd, HWND_BOTTOM, ZBID_DESKTOP);

    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return TRUE;
}

/* 回读验证: 窗口是否真的已变成"非置顶 / 非最大化 / 小窗"
   这是整套兜底的关键 —— 不验证就无法知道到底是成功了还是被还原了。 */
static BOOL VerifyDemoted(HWND hwnd, int screenW, int screenH) {
    RECT    r;
    LONG_PTR ex, st;
    int     w, h;

    if (!IsWindow(hwnd)) return FALSE;
    if (IsIconic(hwnd)) return FALSE;              /* 最小化: 矩形是幽灵值, 不能算成功 */
    if (!IsWindowVisible(hwnd)) return FALSE;      /* 被隐藏 = 不算"小窗化成功" */
    if (!GetWindowRect(hwnd, &r)) return FALSE;

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    st = GetWindowLongPtrW(hwnd, GWL_STYLE);
    if (ex & WS_EX_TOPMOST) return FALSE;
    if ((st & WS_MAXIMIZE) || IsZoomed(hwnd)) return FALSE;

    w = r.right - r.left;
    h = r.bottom - r.top;
    /* 目标宽度是屏幕 1/3; 只要**任一维**还接近全屏就算没降下来。
       以前用 && : 1920x200 这种"全宽窄条"会被判成验证成功, 于是不再加固,
       下一轮 IsAlreadyDemoted 又说没就位 -> 200ms 重设/闪烁的死循环。 */
    if (w >= screenW * 2 / 3 || h >= screenH * 2 / 3) return FALSE;
    return TRUE;
}

/* 分级降级 + 验证; 返回实际达到的 Tier: 0=失败, 1=基础, 2=加固 */
/* 跨进程改窗口会向目标线程发同步消息, 目标卡死时本进程 UI 也会跟着假死。
   先用 SMTO_ABORTIFHUNG 探一下。probeMs 是等待上限:
     - 在 UI 线程上 (FlushPendingDemote) 必须很短 (60ms), 否则一次复核就能
       让窗口"拖不动";
     - 在 worker 线程上可以给 200ms, 少误判"无响应"。 */
static BOOL WindowResponds(HWND hwnd, DWORD probeMs) {
    DWORD_PTR res = 0;
    if (!IsWindow(hwnd)) return FALSE;
    if (SendMessageTimeout(hwnd, WM_NULL, 0, 0,
                           SMTO_ABORTIFHUNG | SMTO_BLOCK, probeMs, &res))
        return TRUE;
    return FALSE;
}

static int DemoteWindowTiered(HWND hwnd, int screenW, int screenH, DWORD probeMs) {
    if (!WindowResponds(hwnd, probeMs)) return 0;   /* 目标无响应: 别把界面拖死 */
    DemoteWindowBase(hwnd, screenW, screenH);
    if (VerifyDemoted(hwnd, screenW, screenH)) {
        return 1;
    }

    DemoteWindowHard(hwnd, screenW, screenH);
    if (VerifyDemoted(hwnd, screenW, screenH)) {
        return 2;
    }

    return 0;   /* 疑似独占全屏: 窗口层已经无能为力 */
}

/* --- 失败跟踪: 同一个窗口反复失败时, 只在第 1 次和每 5 次打日志, 避免刷屏 ---
   注意: 轮询线程 (SmallWinThreadProc) 和 UI 线程都会调用, 用 LogLock 保护。 */
#define MAX_FAIL_TRACK   16
static HWND g_failWin[MAX_FAIL_TRACK];
static int  g_failCount[MAX_FAIL_TRACK];
static int  g_failN = 0;

static int NoteDemoteFailure(HWND hwnd) {
    int i, k = 0, res = 1;
    BOOL found = FALSE;

    LogLock();
    /* 清掉已销毁的窗口, 释放槽位 */
    for (i = 0; i < g_failN; i++) {
        if (g_failWin[i] && IsWindow(g_failWin[i])) {
            g_failWin[k] = g_failWin[i];
            g_failCount[k] = g_failCount[i];
            k++;
        }
    }
    g_failN = k;

    for (i = 0; i < g_failN; i++) {
        if (g_failWin[i] == hwnd) {
            res = ++g_failCount[i];
            found = TRUE;
            break;
        }
    }
    if (!found) {
        if (g_failN < MAX_FAIL_TRACK) {
            g_failWin[g_failN] = hwnd;
            g_failCount[g_failN] = 1;
            g_failN++;
            res = 1;
        } else {
            res = 999;      /* 表满且不认识这个窗口: 不再重复打日志 */
        }
    }
    LogUnlock();
    return res;
}

static void ClearDemoteFailure(HWND hwnd) {
    int i;

    LogLock();
    for (i = 0; i < g_failN; i++) {
        if (g_failWin[i] == hwnd) {
            g_failWin[i] = g_failWin[g_failN - 1];
            g_failCount[i] = g_failCount[g_failN - 1];
            g_failN--;
            break;
        }
    }
    LogUnlock();
}

/* ------------------------------------------------------------------
 * 事件驱动即时小窗化
 *
 * 轮询 (SmallWinThreadProc) 的问题是慢: 广播窗口可能先 全屏+置顶 显示,
 * 0.8 秒后才被改小。这里用 SetWinEventHook(EVENT_OBJECT_SHOW) 在窗口
 * 出现的那一刻就处理, 并用 150ms 定时器复核一次 —— 因为很多程序是
 * ShowWindow() 之后才 Resize 到全屏的, 只处理 SHOW 事件会漏掉最终尺寸。
 * ------------------------------------------------------------------ */

/* 把窗口放进待复核队列 (UI 线程专用, 无需加锁) */
static void QueuePendingDemote(HWND hwnd) {
    for (int i = 0; i < g_pendingDemoteCount; i++) {
        if (g_pendingDemote[i] == hwnd) return;      /* 去重 */
    }
    if (g_pendingDemoteCount >= MAX_PENDING_DEMOTE) {
        memmove(&g_pendingDemote[0], &g_pendingDemote[1],
                sizeof(HWND) * (MAX_PENDING_DEMOTE - 1));
        g_pendingDemoteCount = MAX_PENDING_DEMOTE - 1;
    }
    g_pendingDemote[g_pendingDemoteCount++] = hwnd;
}

/* 复核队列里的窗口并小窗化; 返回处理数量。
   ★ 每次最多真正处理 2 个: 这个函数运行在 UI 线程 (WM_TIMER), 每个窗口
     最坏 200ms 的 WindowResponds + 跨进程 SetWindowPos, 16 个就是 3 秒多的
     消息循环停摆 (界面"假死")。剩下的留到下一拍, 队列非空就再装一次定时器。 */
#define MAX_PER_TICK 2
static int FlushPendingDemote(void) {
    int    n = 0;
    int    handled = 0;
    int    left = 0;
    int    screenW = GetSystemMetrics(SM_CXSCREEN);
    int    screenH = GetSystemMetrics(SM_CYSCREEN);

    for (int i = 0; i < g_pendingDemoteCount; i++) {
        HWND h = g_pendingDemote[i];
        char cls[128] = {0};
        int  tier;

        if (!h || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h)) continue;

        /* 用户主动(模拟)最大化的窗口: 尺寸/标题栏都由用户控制, 不再降级 */
        if (GetPropA(h, "BypassUserMax")) continue;

        /* 只处理渲染器窗口 (学生端自己的工具条/虚拟桌面面板一律不碰) */
        GetClassNameA(h, cls, sizeof(cls));
        if (!IsRenderWindowClass(cls)) {
            DWORD hp = 0;
            char  img[MAX_PATH] = "?";
            GetWindowThreadProcessId(h, &hp);
            GetPidImageName(hp, img, sizeof(img));
            if (!IsRendererImage(img)) continue;
        }

        /* 已就位就别再碰 (重复 SetWindowPos+ShowWindow 会让渲染器每 150ms 闪一次) */
        if (IsAlreadyDemoted(h, screenW, screenH)) continue;

        /* 带标题栏、没铺满屏的普通窗口 (虚拟桌面预览) 一律不碰 */
        if (!IsWindowFullscreen(h, screenW, screenH) &&
            IsPlainWindow(h, screenW, screenH)) continue;

        /* 接管过的窗口只要不是又变回满屏/置顶, 就不动它 —— 用户可能刚
           把它拖大或最大化, 这 150ms 的复核绝不能把人家按回去。
           已最大化 (IsZoomed) 同样放过: 那是用户点的, 不是"又变回全屏"。 */
        if (IsManagedWin(h) &&
            (IsZoomed(h) ||
             (!IsWindowFullscreen(h, screenW, screenH) &&
              !(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOPMOST))))
            continue;

        if (handled >= MAX_PER_TICK) {          /* 剩下的留到下一拍, 别霸占 UI 线程 */
            g_pendingDemote[left++] = h;
            continue;
        }
        handled++;

        GetClassNameA(h, cls, sizeof(cls));
        tier = DemoteWindowTiered(h, screenW, screenH, 60);   /* UI 线程: 短探针 */
        if (tier > 0) {
            n++;
            MarkManagedWin(h);
            ClearDemoteFailure(h);
            AppendLog(g_hwndMain,
                "[SmallWin] instant demote hwnd=0x%p cls='%s' (Tier %d)\r\n",
                (void*)h, cls, tier);
        } else {
            int fc = NoteDemoteFailure(h);
            if (fc == 1 || (fc % 5) == 0) {
                AppendLog(g_hwndMain,
                    "[SmallWin] demote FAILED x%d hwnd=0x%p cls='%s' "
                    "-> likely exclusive fullscreen\r\n",
                    fc, (void*)h, cls);
            }
        }
    }
    g_pendingDemoteCount = left;
    if (left && g_hwndMain)          /* 还有没处理完的: 100ms 后再来一拍 */
        SetTimer(g_hwndMain, IDT_DEMOTE_RECHECK, 100, NULL);
    return n;
}

/* WinEvent 回调: 在安装钩子的线程 (UI 线程) 的消息循环里执行 */
static void CALLBACK BroadcastWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd,
                                           LONG idObject, LONG idChild,
                                           DWORD evThread, DWORD evTime) {
    DWORD   pid = 0;
    char    cls[128] = {0};
    RECT    r;
    LONG_PTR ex;
    int     i, w, h, screenW, screenH;
    BOOL    knownClass = FALSE, big, topmost;

    (void)hook; (void)evThread; (void)evTime;

    if (!g_smallWinRun) return;
    if (!hwnd) return;
    if (idObject != OBJID_WINDOW || idChild != 0) return;
    if (event != EVENT_OBJECT_SHOW) return;
    if (hwnd == g_hwndMain) return;
    if (!IsWindowVisible(hwnd)) return;
    if (GetWindow(hwnd, GW_OWNER) != NULL) return;   /* 跳过对话框/子窗口 */

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return;

    GetClassNameA(hwnd, cls, sizeof(cls));
    for (i = 0; BROADCAST_CLASSES[i]; i++) {
        if (_stricmp(cls, BROADCAST_CLASSES[i]) == 0) { knownClass = TRUE; break; }
    }

    if (!knownClass) {
        RefreshBroadcastPids();
        if (!IsBroadcastPid(pid)) return;
    }

    /* 只动渲染器窗口 (StudentHelperWnd 之类是学生自己的界面, 不能套小窗) */
    if (!knownClass && !IsRenderWindowClass(cls)) {
        char img[MAX_PATH] = "?";
        GetPidImageName(pid, img, sizeof(img));
        if (!IsRendererImage(img)) return;
    }

    if (!GetWindowRect(hwnd, &r)) return;
    w = r.right - r.left;
    h = r.bottom - r.top;
    if (w < 160 || h < 120) return;                  /* 排除悬浮条/IME 等 */

    screenW = GetSystemMetrics(SM_CXSCREEN);
    screenH = GetSystemMetrics(SM_CYSCREEN);

    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    topmost = (ex & WS_EX_TOPMOST) != 0;
    big = (w >= screenW * 2 / 3) && (h >= screenH * 2 / 3);

    /* 带标题栏、又没铺满屏 -> 普通窗口 (虚拟桌面预览), 不是全屏广播 */
    if (!IsWindowFullscreen(hwnd, screenW, screenH) &&
        IsPlainWindow(hwnd, screenW, screenH)) return;

    if (!knownClass && !big && !topmost) return;

    /* 完全接管过的窗口: 除非又被人改回置顶, 否则不重新摆位置。
       用户可能刚把它拖大/最大化, SHOW 事件不该把它按回小窗。
       标题栏自动隐藏后没有 WS_CAPTION, 也不能重新降级 (会破坏自动隐藏)。 */
    if (GetPropA(hwnd, "BypassUserMax")) return;
    if (IsManagedWin(hwnd) && !topmost) return;

    /* 1) 先立即处理一次 (窗口出现时往往已是最终尺寸), 并回读验证 */
    if (DemoteWindowTiered(hwnd, screenW, screenH, 200) > 0) {
        MarkManagedWin(hwnd);
        ClearDemoteFailure(hwnd);
    } else {
        int fc = NoteDemoteFailure(hwnd);
        if (fc == 1 || (fc % 5) == 0) {
            AppendLog(g_hwndMain,
                "[SmallWin] demote FAILED x%d hwnd=0x%p cls='%s' "
                "-> likely exclusive fullscreen; if the renderer keeps "
                "forcing fullscreen, restart it after T1.5 preset\r\n",
                fc, (void*)hwnd, cls);
        }
    }

    /* 2) 再排队 150ms 后复核一次, 兜住 "先 Show 再 Resize 到全屏" 的情况。
       ★ 只在队列原本为空时装定时器: 每来一个 SHOW 事件就 SetTimer(150) 会
         把复核一直往后推 (Windows 的定时器是重置而不是叠加)。 */
    {
        BOOL wasEmpty = (g_pendingDemoteCount == 0);

        QueuePendingDemote(hwnd);
        if (wasEmpty) SetTimer(g_hwndMain, IDT_DEMOTE_RECHECK, 150, NULL);
    }
}

static void InstallBroadcastHook(HWND hwndLog) {
    if (g_hBroadcastHook) return;
    if (!g_hwndMain) return;      /* 无消息循环(静默模式)时只能靠轮询兜底 */

    g_hBroadcastHook = SetWinEventHook(
        EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW,
        NULL, BroadcastWinEventProc, 0, 0,
        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    if (g_hBroadcastHook) {
        AppendLog(hwndLog,
            "[SmallWin] WinEvent hook installed (instant demote on window show)\r\n");
    } else {
        AppendLog(hwndLog,
            "[SmallWin] WARNING: SetWinEventHook failed (err=%lu); polling fallback only\r\n",
            GetLastError());
    }
    g_pendingDemoteCount = 0;
}

static void RemoveBroadcastHook(void) {
    if (g_hBroadcastHook) {
        UnhookWinEvent(g_hBroadcastHook);
        g_hBroadcastHook = NULL;
    }
    if (g_hwndMain) KillTimer(g_hwndMain, IDT_DEMOTE_RECHECK);
    g_pendingDemoteCount = 0;
}

static BOOL CALLBACK DemoteEnumProc(HWND hwnd, LPARAM lp) {
    DemoteCtx* ctx = (DemoteCtx*)lp;
    DWORD pid = 0;
    RECT r;
    char cls[128] = {0};
    int i, w, h;
    BOOL matched, knownClass, fullscreen, big, topmost;
    LONG_PTR ex;

    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0 || pid == GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(hwnd)) return TRUE;
    /* 最小化在任务栏里的窗口不要碰 (和事件路径 FlushPendingDemote 保持一致) */
    if (IsIconic(hwnd)) return TRUE;
    /* 有 owner 的顶层窗口是对话框/浮层 (渲染器也会弹), 不是广播画面 */
    if (GetWindow(hwnd, GW_OWNER) != NULL) return TRUE;

    GetClassNameA(hwnd, cls, sizeof(cls));
    knownClass = FALSE;
    for (i = 0; BROADCAST_CLASSES[i]; i++) {
        if (_stricmp(cls, BROADCAST_CLASSES[i]) == 0) { knownClass = TRUE; break; }
    }

    /* 类名命中就不再要求 PID 匹配: 进程被改名/换壳时也能抓到
       (原实现把 knownClass 算出来了却没用, 等于白算) */
    matched = FALSE;
    for (i = 0; i < ctx->npid; i++) {
        if (ctx->pids[i] == pid) { matched = TRUE; break; }
    }
    if (!matched && !knownClass) return TRUE;

    /* ★ 只动渲染器的窗口: Student.exe 的 StudentHelperWnd 是学生自己的界面,
       套上小窗就会变成"软件把虚拟桌面包成小窗"。 */
    if (!knownClass && !IsRenderWindowClass(cls)) {
        char img[MAX_PATH] = "?";
        GetPidImageName(pid, img, sizeof(img));
        if (!IsRendererImage(img)) return TRUE;
    }

    if (!GetWindowRect(hwnd, &r)) return TRUE;
    w = r.right - r.left;
    h = r.bottom - r.top;
    if (w < 160 || h < 120) return TRUE;   /* 排除悬浮条/IME 等 */

    fullscreen = IsWindowFullscreen(hwnd, ctx->screenW, ctx->screenH);
    ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    topmost = (ex & WS_EX_TOPMOST) != 0;
    big = (w >= ctx->screenW * 2 / 3) && (h >= ctx->screenH * 2 / 3);

    /* ★ 不是满屏 + 已经有标题栏 = 已经是个普通窗口 (虚拟桌面预览窗就长这样:
       1344x637 + 置顶 + 带框)。用户能自己拖它, 我们再去降级只会多出一个
       小窗 —— 实测就是这样冒出来"两个小窗"的。 */
    if (!fullscreen && IsPlainWindow(hwnd, ctx->screenW, ctx->screenH)) return TRUE;

    /* 用户主动(模拟)最大化的广播窗: 已经接管, 尺寸是用户要的。
       它的标题栏可能已被自动隐藏 (没有 WS_CAPTION), 绝不能再降级一次。 */
    if (GetPropA(hwnd, "BypassUserMax")) {
        ctx->small++;
        return TRUE;
    }

    /* ★ 已在目标形态 -> 直接跳过, 一个 SetWindowPos 都不发。
       少了这一句, 轮询每 200ms 就会把窗口重新摆一次, 渲染器跟着重建表面,
       表现就是"界面每 200ms 黑闪一下"。 */
    if (IsAlreadyDemoted(hwnd, ctx->screenW, ctx->screenH)) {
        if (IsManagedWin(hwnd)) ctx->small++;   /* 已接管的小窗 -> 自动解锁键盘 */
        return TRUE;
    }

    /* 接管过的窗口: 只处理"又变回满屏/置顶"的情况。
       用户自己拖大、最大化、挪位置 —— 一律放手, 不再按回右下角。
       ★ IsZoomed 这一条是"最大化后日志被 keep user geometry 刷屏"的根:
         最大化后的窗口满足"满屏"判定, 于是每 200ms 又来降级一次, 每次都被
         DLL 的几何冻结挡回去。已接管 + 处于最大化 = 用户自己点的, 直接放过。 */
    if (IsManagedWin(hwnd) &&
        (IsZoomed(hwnd) || (!fullscreen && !topmost))) {
        if (!IsZoomed(hwnd)) ctx->small++;
        return TRUE;
    }

    if (knownClass || fullscreen || big || topmost) {
        int tier;

        /* ★ 计数放在所有"故意跳过"的分支之后: 用户自己最大化的窗口也是
           "满屏", 以前把它算进来, 于是轮询永远停在 200ms 档, 还每 2 秒打一条
           误导性的 "likely exclusive fullscreen"。 */
        if (fullscreen) ctx->fullscreen++;

        tier = DemoteWindowTiered(hwnd, ctx->screenW, ctx->screenH, 200);
        if (tier > 0) {
            RECT rr;
            MarkManagedWin(hwnd);
            ctx->demoted++;
            ctx->small++;
            if (!ctx->firstHwnd) ctx->firstHwnd = hwnd;
            if (GetWindowRect(hwnd, &rr)) {
                ctx->lastW = rr.right - rr.left;
                ctx->lastH = rr.bottom - rr.top;
            }
            ClearDemoteFailure(hwnd);
            if (!ctx->quiet && ctx->logWnd) {
                AppendLog(ctx->logWnd,
                    "[SmallWin] demoted hwnd=0x%p cls='%s' %dx%d (Tier %d)\r\n",
                    (void*)hwnd, cls, w, h, tier);
            }
        } else {
            int fc = NoteDemoteFailure(hwnd);
            if (!ctx->quiet && ctx->logWnd && (fc == 1 || (fc % 5) == 0)) {
                AppendLog(ctx->logWnd,
                    "[SmallWin] demote FAILED x%d hwnd=0x%p cls='%s' %dx%d "
                    "-> likely exclusive fullscreen\r\n",
                    fc, (void*)hwnd, cls, w, h);
            }
        }
    }
    return TRUE;
}

/* 执行一次小窗化; 返回处理数量. quiet=TRUE 时少打日志 (供线程/定时调用) */
static int SmallWindowBroadcastCore(HWND hwndLog, BOOL quiet) {
    DemoteCtx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.quiet = quiet;
    ctx.logWnd = hwndLog;
    ctx.screenW = GetSystemMetrics(SM_CXSCREEN);
    ctx.screenH = GetSystemMetrics(SM_CYSCREEN);

    if (!CollectBroadcastPids(&ctx)) {
        if (!quiet && hwndLog)
            AppendLog(hwndLog,
                "[SmallWin] no broadcast process running\r\n");
        return 0;
    }

    EnumWindows(DemoteEnumProc, (LPARAM)&ctx);

    if (!quiet && hwndLog) {
        if (ctx.demoted > 0)
            AppendLog(hwndLog,
                "[SmallWin] demoted %d window(s), now movable\r\n",
                ctx.demoted);
        else
            AppendLog(hwndLog,
                "[SmallWin] no fullscreen/topmost broadcast window found\r\n");
    }
    return ctx.demoted;
}

/* ==========================================================================
 *  DLL 注入: 让渲染进程"自己"没法把广播窗口做成全屏/置顶
 * --------------------------------------------------------------------------
 *  实证:
 *    MultiRender.exe  FUN_0040a790 / FUN_0040ac10
 *        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, cx, cy, 0)
 *        SetWindowLongW(hwnd, GWL_STYLE, style & 0xFF3BFFFF | WS_POPUP)
 *        ShowCursor(0)
 *    MultiRender.exe  FUN_0040b780
 *        每次 ShowWindow 之后用 this+0x8ac 里存的 -1 重新套 HWND_TOPMOST
 *    ScreenRender.exe FUN_00411160
 *        500ms 的 WM_TIMER 里再套一次 HWND_TOPMOST, 然后 KillTimer
 *
 *  这些调用全部走进程自己的 user32 导入表。bypass_hook32.dll 把这些 IAT 项
 *  换成自己的函数, 于是:
 *    1) 窗口从创建那一刻就是小窗, 不是我们 200ms 后再去抢;
 *    2) 渲染器自己算出来的客户区也是小的, 不会按全屏分辨率出图;
 *    3) 没有反复重绘, 所以不会闪。
 *
 *  ⚠ 三个目标进程都是 PE32 (x86)。64 位进程不能给 32 位进程注入 32 位 DLL,
 *    所以这里调用同目录下的 32 位 inject32.exe 去注入。
 *
 *  总开关: 关闭「持续保持小窗」时会在同目录写 bypass_hook.off,
 *          DLL 看到该文件就把所有钩子放行 (不必重启渲染进程)。
 *
 *  ⚠ 已知局限 (IAT hook 的边界, 实测定论于 32 位 hooktest32.exe):
 *    IAT hook 只能改"每次调用都重新读导入表"的调用点。若编译器把 IAT 槽里
 *    的函数指针提前读进寄存器并跨循环复用 (GCC -O2 就会这么做, 实测
 *    `mov 0x408324,%edi` 之后就一直是 `call *%edi`), 注入后改 IAT 对
 *    那个调用点无效 —— 窗口层降级 (DemoteWindowTiered) 仍然兜底, 但这种
 *    情况下会退回到"每轮抢一次"的行为, 可能看到轻微闪动。
 *    厂商二进制由 MSVC 编译, 调用点通常是 `call dword ptr [__imp_xxx]`,
 *    每次重读 IAT, 所以 IAT hook 应当有效。若实测「注入成功但窗口照样满屏」,
 *    说明该调用点被优化掉了, 需要升级成 inline hook (改 user32 函数入口)。
 * ========================================================================== */

static DWORD g_lastInjectTick = 0;
static DWORD g_lastTargetHash = 0;

/* 课堂进程是否出现了 + 它们的 PID 集合哈希。
   用途: 课堂软件可能在 bypass 之后才启动, 必须"一出现就注入",
   不能等 5 秒轮询 —— 否则 Student 拉起 MultiClient 时钩子还没进去。 */
static DWORD TargetPidHash(void)
{
    static const char* names[] = {
        "ScreenRender.exe", "MultiRender.exe", "MultiClient.exe",
        "AudioRender.exe", "AudioSender.exe", "Student.exe",
        "MmcStudent.exe",
        "SharedDesktop.exe", "StudentScreenHelper.exe", "ScreenSender.exe",
        "hooktest32.exe", NULL
    };
    HANDLE snap;
    PROCESSENTRY32 pe;
    DWORD h = 0;
    int i;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            for (i = 0; names[i]; i++) {
                if (_stricmp(pe.szExeFile, names[i]) == 0) {
                    h += pe.th32ProcessID * 2654435761u;   /* Knuth 常数, 只求变化 */
                    break;
                }
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return h;
}

/* 把 inject32 的 stdout 按整行转进主日志 (最多 60 行)。
   ★ 不再经过磁盘: inject32 不传日志路径就不创建文件, 输出走匿名管道。 */
static void AppendInjectText(HWND hwndLog, const char* text) {
    const char* p = text;
    int shown = 0;

    if (!text) return;
    while (*p && shown < 60) {
        const char* e = p;
        const char* stop;
        char line[512];
        int n;

        while (*e && *e != '\n') e++;
        stop = e;
        while (stop > p && stop[-1] == '\r') stop--;

        n = (int)(stop - p);
        if (n > 0) {
            if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
            memcpy(line, p, n);
            line[n] = '\0';
            AppendLog(hwndLog, "[Hook] %s\r\n", line);
            shown++;
        }
        if (!*e) break;
        p = e + 1;
    }
}

/* 写/删 bypass_hook.off (DLL 的同目录)。
   ★ 它的语义是"放行": 已注入的 DLL 读到它就停止干预窗口 (渲染进程不用重启)。
     只在**用户明确要求放行**时写 (关闭"持续保持小窗" / 托盘里的"恢复全屏广播")。
     退出程序不写 —— 否则每次关掉工具都会留下一个 .off, 下次启动前那段窗口
     里 DLL 是被禁用的, 看起来就像"小窗化没生效"。 */
static void SetHookOffFlag(BOOL off) {
    char dir[MAX_PATH];
    char flag[MAX_PATH];

    GetModuleFileName(NULL, dir, MAX_PATH);
    PathRemoveFileSpec(dir);
    PathCombine(flag, dir, "bypass_hook.off");

    if (off) {
        HANDLE h = CreateFileA(flag, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    } else {
        DeleteFileA(flag);
    }
}

/* 把 bypass_hook32.dll 注入到所有正在运行的渲染进程里。
   verbose = TRUE  用户主动触发: 缺文件也报警, 5 秒节流不生效
   verbose = FALSE 轮询线程调用: 每 5 秒最多一次, 没动静就不刷日志 */
void RunInjector(HWND hwndLog, BOOL verbose) {
    char dir[MAX_PATH];
    char injector[MAX_PATH];
    char dllPath[MAX_PATH];
    char cmdLine[MAX_PATH * 2 + 64];
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE rd = NULL, wr = NULL;
    HANDLE nulIn = INVALID_HANDLE_VALUE;
    DWORD now = GetTickCount();
    char  out[8192];
    DWORD total = 0;
    DWORD start;
    BOOL  ok;

    if (!verbose) {
        DWORD th;

        /* ★ 先看时间再拉快照: 200ms 节拍下以前每秒做 5 次整表进程快照。 */
        if (g_lastInjectTick && (now - g_lastInjectTick) < 2000) return;

        th = TargetPidHash();

        /* 没有课堂进程: 不启动 inject32 (省得每轮拉一个进程)。
           同时清掉"上次的 PID 集合": 进程集合清空后再出现, 要能立刻注入。 */
        if (!th) {
            g_lastTargetHash = 0;
            g_lastInjectTick = now;
            return;
        }
        /* PID 集合没变: 15 秒复查一次即可; 变了(有新进程) -> 立刻注入 */
        if (th == g_lastTargetHash && (now - g_lastInjectTick) < 15000) {
            return;
        }
        g_lastTargetHash = th;
    }
    g_lastInjectTick = now;

    GetModuleFileName(NULL, dir, MAX_PATH);
    PathRemoveFileSpec(dir);

    PathCombine(injector, dir, "inject32.exe");
    PathCombine(dllPath, dir, "bypass_hook32.dll");

    if (GetFileAttributesA(injector) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesA(dllPath) == INVALID_FILE_ATTRIBUTES) {
        if (verbose) {
            AppendLog(hwndLog,
                "[Hook] inject32.exe / bypass_hook32.dll 不在本目录, "
                "跳过 DLL 注入 (只剩窗口层降级可用)\r\n");
        }
        return;
    }

    /* ★ 不再传日志文件路径: inject32 只有在 argv[2] 存在时才创建文件。
       它的输出走 stdout, 这里用匿名管道接住, 再转进界面日志 —— 磁盘上不留文件。 */
    _snprintf(cmdLine, sizeof(cmdLine) - 1, "\"%s\" \"%s\"", injector, dllPath);
    cmdLine[sizeof(cmdLine) - 1] = '\0';

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) { rd = wr = NULL; }
    if (rd) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    /* ★ 只有真的拿到管道时才声明 STARTF_USESTDHANDLES: GUI 进程里
       GetStdHandle 通常是 NULL, 而 STARTF_USESTDHANDLES + 无效句柄会让
       CreateProcess 直接失败 (ERROR_INVALID_HANDLE)。stdin 给 NUL。 */
    if (wr) {
        nulIn = CreateFileA("NUL", GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                            OPEN_EXISTING, 0, NULL);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = (nulIn != INVALID_HANDLE_VALUE) ? nulIn : wr;
        si.hStdOutput = wr;
        si.hStdError  = wr;
    }

    memset(&pi, 0, sizeof(pi));
    ok = CreateProcessA(NULL, cmdLine, NULL, NULL, wr ? TRUE : FALSE,
                        CREATE_NO_WINDOW, NULL, dir, &si, &pi);
    if (nulIn != INVALID_HANDLE_VALUE) CloseHandle(nulIn);
    if (wr) { CloseHandle(wr); wr = NULL; }

    if (!ok) {
        if (rd) CloseHandle(rd);
        AppendLog(hwndLog, "[Hook] 启动 inject32.exe 失败 (err=%lu)\r\n",
                  GetLastError());
        return;
    }

    /* 边跑边读管道, 避免子进程写满管道阻塞 */
    start = GetTickCount();
    for (;;) {
        DWORD avail = 0;

        if (rd && PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
            DWORD want = (DWORD)sizeof(out) - 1 - total;
            DWORD got = 0;
            if (want > avail) want = avail;
            if (want > 0 && ReadFile(rd, out + total, want, &got, NULL) && got)
                total += got;
        }

        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;

        /* ★ 用户关掉了「持续保持小窗」: 别再等最多 8 秒的注入器。
           它只负责注入, 杀掉不影响已经加载进目标进程的 DLL。 */
        if (!g_smallWinRun) {
            AppendLog(hwndLog, "[Hook] sticky mode is off - aborting inject32.exe\r\n");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 500);
            break;
        }

        if (GetTickCount() - start > 8000) {
            AppendLog(hwndLog, "[Hook] inject32.exe 超时, 强制结束\r\n");
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 500);
            break;
        }
        if (total >= sizeof(out) - 1) {      /* 输出过多: 不再读, 等它自己结束 */
            if (WaitForSingleObject(pi.hProcess, 8000) != WAIT_OBJECT_0) {
                /* 不结束就杀掉: 句柄一关它就成了孤儿, 还卡在写管道上 */
                AppendLog(hwndLog, "[Hook] inject32.exe 输出过多且未退出, 强制结束\r\n");
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 500);
            }
            break;
        }
    }

    /* 进程退出后把管道里剩下的读干净 */
    if (rd) {
        for (;;) {
            DWORD avail = 0, got = 0, want;
            if (!PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) || avail == 0) break;
            want = (DWORD)sizeof(out) - 1 - total;
            if (want > avail) want = avail;
            if (want == 0) break;
            if (!ReadFile(rd, out + total, want, &got, NULL) || got == 0) break;
            total += got;
        }
        CloseHandle(rd);
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    out[total] = '\0';

    if (verbose) {
        AppendLog(hwndLog, "[Hook] inject32.exe 退出, 输出如下:\r\n");
        AppendInjectText(hwndLog, out);
    } else {
        /* 轮询线程每 5 秒跑一次: 只有输出真的变了才转贴, 避免刷屏 */
        static DWORD lastHash;
        DWORD h = 2166136261u;
        DWORD i;

        for (i = 0; i < total; i++)
            h = (h ^ (unsigned char)out[i]) * 16777619u;

        if (total && h != lastHash) {
            lastHash = h;
            AppendInjectText(hwndLog, out);
        }
    }
}

/* 兜底轮询线程: 钩子事件之外的情况 (窗口在钩子安装前就存在, 或被改回去)。
   节拍自适应, 并且**有退让机制**:
     已就位 / 无满屏        -> 1.2s
     刚降级过               -> 200ms 复核一次是否被改回去
     连续 4 轮都在重新降级  -> 说明渲染器每次都改回去, 我们根本赢不了:
                              停止干预 (继续干预只会继续闪), 只在日志里提醒

   ⚠ 实测 (2026-10-22): 这里绝对不能主动 SwitchDesktop。
   MultiClient.exe 自己会 SwitchDesktop("NetTeachingDesktop"), 我们再切回
   "Default", 两边互相打架, 每次切换都是一次整屏重绘 —— 用户看到的就是
   "黑屏闪动"。回到用户桌面改为只提示, 由用户按 Ctrl+Alt+D 决定时机。 */
DWORD WINAPI SmallWinThreadProc(LPVOID lp) {
    HWND  logWnd = (HWND)lp;
    DWORD lastDesktopWarn = 0;
    DWORD lastDemoteLog   = 0;
    DWORD lastGiveUpLog   = 0;
    int   fightStreak     = 0;   /* 同一个窗口连续被降级几次 = 渲染器在反复改回去 */
    HWND  lastFightHwnd   = NULL;
    BOOL  gaveUp          = FALSE;

    SafeLog("[SmallWin] fullscreen watcher started (adaptive, with back-off)\r\n");

    /* 首轮准备工作 (以前在 UI 线程同步做 -> 点按钮白屏几秒):
       放行标志归零 -> 注入 -> 兼容标志预设 -> 全量扫描。
       每一步都检查 g_smallWinRun: 用户可能刚点上就关掉。 */
    if (g_smallWinRun) {
        SetHookOffFlag(FALSE);
        AppendLog(logWnd, "[Hook] 注入 bypass_hook32.dll 到渲染进程...\r\n");
        RunInjector(logWnd, TRUE);
    }
    if (g_smallWinRun) PresetFullscreenDowngrade(logWnd, TRUE);
    if (g_smallWinRun) SmallWindowBroadcastCore(logWnd, TRUE);
    while (g_smallWinRun) {
        DemoteCtx ctx;
        int   step;
        DWORD now = GetTickCount();
        char  dname[128];

        /* 新起来的渲染进程也要 hook 上 (内部自己节流到 5s 一次) */
        RunInjector(NULL, FALSE);

        memset(&ctx, 0, sizeof(ctx));
        ctx.quiet = TRUE;
        ctx.screenW = GetSystemMetrics(SM_CXSCREEN);
        ctx.screenH = GetSystemMetrics(SM_CYSCREEN);

        if (gaveUp) {
            /* 已经认输: 只观察, 绝不再碰窗口 (再碰就是继续闪)。
               ★ 每轮用一次进程快照判断"这一场是不是结束了": 渲染器没了就
                 解除 back-off (以前 gaveUp 一旦置位只能重启工具)。 */
            if (now - lastGiveUpLog > 30000) {
                lastGiveUpLog = now;
                SafeLog("[SmallWin] watcher idle: renderer keeps restoring "
                        "fullscreen. Use 'Kill classroom processes' "
                        "(or Ctrl+Alt+X) to stop it for real.\r\n");
            }
            {
                DemoteCtx probe;
                memset(&probe, 0, sizeof(probe));
                probe.quiet = TRUE;
                if (!CollectBroadcastPids(&probe)) {
                    gaveUp = FALSE;
                    fightStreak = 0;
                    lastFightHwnd = NULL;
                    SafeLog("[SmallWin] back-off cleared: no broadcast process left\r\n");
                }
            }
            step = 3000;
        } else {
            if (CollectBroadcastPids(&ctx)) {
                EnumWindows(DemoteEnumProc, (LPARAM)&ctx);
            }

            /* ★ 只统计"同一个窗口被反复改回去": 渲染器每轮新建窗口是正常行为,
               以前按 ctx.demoted>0 累加, 连续弹 4 个窗口就永久放弃。 */
            if (ctx.demoted > 0) {
                if (ctx.firstHwnd && ctx.firstHwnd == lastFightHwnd) fightStreak++;
                else                                                  fightStreak = 1;
                lastFightHwnd = ctx.firstHwnd;
            } else {
                fightStreak = 0;
                lastFightHwnd = NULL;
            }

            if (now - lastDemoteLog > 2000) {
                if (ctx.demoted > 0) {
                    lastDemoteLog = now;
                    SafeLog("[SmallWin] watcher: demoted %d, hwnd=0x%p -> now %dx%d "
                            "(same hwnd => re-asserted, new hwnd => recreated)\r\n",
                            ctx.demoted, (void*)ctx.firstHwnd,
                            ctx.lastW, ctx.lastH);
                } else if (ctx.fullscreen > 0) {
                    lastDemoteLog = now;
                    SafeLog("[SmallWin] watcher: %d fullscreen window(s) seen but "
                            "none demoted -> likely exclusive fullscreen\r\n",
                            ctx.fullscreen);
                }
            }

            if (fightStreak >= 4) {
                gaveUp = TRUE;
                SafeLog("[SmallWin] BACK-OFF: the renderer undid the demote %d times "
                        "in a row. Stopping automatic intervention so the screen "
                        "stops flickering. Kill the render process to win.\r\n",
                        fightStreak);
                step = 3000;
            } else {
                step = (ctx.fullscreen > 0 || ctx.demoted > 0) ? 200 : 1200;
                /* 还没见过课堂进程 -> 加快扫描, 保证"课堂软件后启动"也能在
                   它拉起 MultiClient 之前注进去 (否则 SwitchDesktop 抢先)。 */
                if (!g_lastTargetHash) step = 400;
            }
        }

        /* 私有桌面: 只提示, 不切换 (切换会和 MultiClient 打架 -> 黑屏闪动) */
        if (GetInputDesktopName(dname, sizeof(dname)) && dname[0] &&
            _stricmp(dname, DEFAULT_DESKTOP_NAME) != 0 &&
            _stricmp(dname, LOCK_DESKTOP_NAME) != 0) {
            if (now - lastDesktopWarn > 10000) {
                lastDesktopWarn = now;
                SafeLog("[SmallWin] NOTE: input desktop is '%s'; the broadcast "
                        "window lives on another desktop. Ctrl+Alt+X sends an "
                        "emergency click (no auto-switching: it fights "
                        "MultiClient and flashes)\r\n", dname);
            }
        }

        for (int i = 0; i < step / 100 && g_smallWinRun; i++) Sleep(100);
    }

    SafeLog("[SmallWin] fullscreen watcher stopped\r\n");
    return 0;
}

void StartSmallWin(HWND hwnd) {
    if (g_smallWinRun) return;

    /* 上一轮的 watcher 可能还没退干净 (StopSmallWin 等超时过):
       先收它, 否则两个线程同时 EnumWindows + SetWindowPos = 闪。 */
    if (g_hSmallWinThread) {
        if (WaitForSingleObject(g_hSmallWinThread, 1500) == WAIT_OBJECT_0) {
            CloseHandle(g_hSmallWinThread);
            g_hSmallWinThread = NULL;
        } else {
            AppendLog(hwnd, "[SmallWin] previous watcher is still busy; "
                            "not starting a second one\r\n");
            return;
        }
    }

    g_smallWinRun = TRUE;

    /* 事件驱动: 广播窗口一出现就处理 (即时)。
       ★ 必须在 UI 线程装: WINEVENT_OUTOFCONTEXT 的事件投递到"装钩子的线程"。 */
    InstallBroadcastHook(hwnd);

    /* ★ 注入 / 兼容标志预设 / 首轮全量扫描都挪进 watcher 线程 (见
       SmallWinThreadProc 开头): 它们要跑进程枚举 + CreateProcess + 等
       inject32 (最坏 8 秒), 在 UI 线程同步做就是"点了按钮白屏几秒"。 */

    /* 轮询兜底 (也是首轮准备工作的执行者) */
    g_hSmallWinThread = CreateThread(NULL, 0, SmallWinThreadProc, hwnd, 0, NULL);
    if (!g_hSmallWinThread) {
        AppendLog(hwnd, "[SmallWin] WARNING: watcher thread failed (err=%lu) - "
                        "only the WinEvent hook is active\r\n", GetLastError());
        return;      /* g_smallWinRun 保持 TRUE: 钩子仍在工作 */
    }

    AppendLog(hwnd,
        "[SmallWin] sticky small-window mode ENABLED "
        "(T1.5 preset + WinEvent + adaptive 200ms/1.2s watcher)\r\n");
    UpdateStatus(hwnd, "Status: auto-demote watcher ON");
}

void StopSmallWin(HWND hwnd) {
    if (!g_smallWinRun) return;

    g_smallWinRun = FALSE;

    RemoveBroadcastHook();

    if (g_hSmallWinThread) {
        /* ★ 超时后**不要**关句柄/置 NULL: 线程可能正卡在 RunInjector (最长
           8 秒) 或 WindowResponds (每窗 200ms)。关掉句柄再点开就会起第二个
           watcher; 而且卡住的那一轮回来时还会再降级一次 (用户已经关掉了)。 */
        if (WaitForSingleObject(g_hSmallWinThread, 3000) == WAIT_OBJECT_0) {
            CloseHandle(g_hSmallWinThread);
            g_hSmallWinThread = NULL;
        } else {
            AppendLog(hwnd, "[SmallWin] watcher still busy - it will exit after "
                            "the current pass (not starting another one)\r\n");
        }
    }

    /* ⚠ 这里不写 bypass_hook.off: 退出程序 ≠ 要求放行。
       想恢复全屏广播请用托盘菜单里的「恢复全屏广播（放行）」。 */
    AppendLog(hwnd,
        "\r\n[SmallWin] sticky small-window mode DISABLED\r\n");
    UpdateStatus(hwnd, "Status: sticky small-window OFF");
}

/* ========== 实时监控 / 自保护线程 ========== */



/* ========== UI drawing helpers ========== */

static void CreateUIFonts(void) {
    int dpi = (int)g_dpi;

    if (g_hFontBtn) return;

    g_hFontBtn = CreateFontA(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    g_hFontBtnBold = CreateFontA(-MulDiv(10, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    g_hFontTitle = CreateFontA(-MulDiv(11, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    g_hFontSection = CreateFontA(-MulDiv(8, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    g_hFontLog = CreateFontA(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Consolas");
    g_hFontHint = CreateFontA(-MulDiv(8, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");

    g_hbrBg       = CreateSolidBrush(UI_BG);
    g_hbrTitleBg  = CreateSolidBrush(UI_TITLE_BG);
    g_hbrBorder   = CreateSolidBrush(UI_BORDER);
    g_hbrInputBg  = CreateSolidBrush(UI_INPUT_BG);
    g_hbrLogBg    = CreateSolidBrush(UI_LOG_BG);
    g_hbrCloseHv  = CreateSolidBrush(UI_CLOSE_HV);
    g_hbrMinHv    = CreateSolidBrush(UI_MIN_HV);
}

static void DestroyUIFonts(void) {
    if (g_hFontBtn)     DeleteObject(g_hFontBtn);
    if (g_hFontBtnBold) DeleteObject(g_hFontBtnBold);
    if (g_hFontTitle)   DeleteObject(g_hFontTitle);
    if (g_hFontSection) DeleteObject(g_hFontSection);
    if (g_hFontLog)     DeleteObject(g_hFontLog);
    if (g_hFontHint)    DeleteObject(g_hFontHint);
    if (g_hbrBg)        DeleteObject(g_hbrBg);
    if (g_hbrTitleBg)   DeleteObject(g_hbrTitleBg);
    if (g_hbrBorder)    DeleteObject(g_hbrBorder);
    if (g_hbrInputBg)   DeleteObject(g_hbrInputBg);
    if (g_hbrLogBg)     DeleteObject(g_hbrLogBg);
    if (g_hbrCloseHv)   DeleteObject(g_hbrCloseHv);
    if (g_hbrMinHv)     DeleteObject(g_hbrMinHv);
    g_hFontBtn = g_hFontBtnBold = g_hFontTitle = NULL;
    g_hFontSection = g_hFontLog = g_hFontHint = NULL;
    g_hbrBg = g_hbrTitleBg = g_hbrBorder = NULL;
    g_hbrInputBg = g_hbrLogBg = g_hbrCloseHv = g_hbrMinHv = NULL;
}

static void FillRoundedRect(HDC hdc, RECT rc, int radius, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, br);
    HPEN   op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
}

static void FrameRoundedRect(HDC hdc, RECT rc, int radius, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HPEN op = (HPEN)SelectObject(hdc, pen);
    HBRUSH ob = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(pen);
}

static const BTNDEF* GetBtnDef(int id) {
    int n = (int)(sizeof(g_btnDefs) / sizeof(g_btnDefs[0]));
    for (int i = 0; i < n; i++) {
        if (g_btnDefs[i].id == id) return &g_btnDefs[i];
    }
    return NULL;
}

static BTNSTATE* GetBtnState(int id) {
    int idx = id - 1000;
    if (idx < 0 || idx >= BTN_MAX) idx = 0;
    return &g_btnSt[idx];
}

/* ---- 悬停说明: 把按钮的一句话说明放进右下角状态区 --------------------------
 * 说明直接取自 BTNDEF.desc, 因此"按钮名称/说明/行为"三者在源码里是同一处定义。
 * 带全局快捷键的按钮, 说明后面会自动附上 "Ctrl+Alt+X" 这类提示 —— 不用去翻文档。
 * 用 g_hoverHintId 记录当前归属, 避免鼠标在两个相邻按钮间移动时,
 * 旧按钮的 WM_MOUSELEAVE 晚于新按钮的 WM_MOUSEMOVE 到达而误清说明。 */
static void RepaintStatusArea(void) {
    if (g_hwndMain) InvalidateRect(g_hwndMain, &g_rcStatusArea, FALSE);
}

static void SetHoverHint(int ctlId) {
    const BTNDEF* def = GetBtnDef(ctlId);
    const wchar_t* desc = (def && def->desc) ? def->desc : L"";
    wchar_t text[200];

    if (def && def->hot)
        _snwprintf(text, 199, L"%s    [%s]", desc, def->hot);
    else
        wcsncpy(text, desc, 199);
    text[199] = L'\0';

    /* 只在说明真正变化时才重绘状态区, 避免鼠标移动时反复刷新 */
    if (g_hoverHintId == ctlId && wcscmp(g_hoverHint, text) == 0) return;

    wcsncpy(g_hoverHint, text, 199);
    g_hoverHint[199] = L'\0';
    g_hoverHintId = ctlId;
    RepaintStatusArea();
}

static void ClearHoverHint(int ctlId) {
    if (g_hoverHintId != ctlId) return;   /* 已经切到别的按钮, 不误清 */
    g_hoverHintId = 0;
    g_hoverHint[0] = L'\0';
    RepaintStatusArea();
}

/* button subclass: track mouse hover + 显示按钮功能说明 */
static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l,
                                UINT_PTR uId, DWORD_PTR dw) {
    (void)uId; (void)dw;
    int ctlId = GetDlgCtrlID(h);
    BTNSTATE* st = GetBtnState(ctlId);
    switch (m) {
        case WM_MOUSEMOVE:
            if (!st->hover) {
                TRACKMOUSEEVENT tme;
                st->hover = TRUE;
                InvalidateRect(h, NULL, TRUE);
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = h;
                tme.dwHoverTime = 0;
                TrackMouseEvent(&tme);
            }
            SetHoverHint(ctlId);
            break;
        case WM_MOUSELEAVE:
            st->hover = FALSE;
            InvalidateRect(h, NULL, TRUE);
            ClearHoverHint(ctlId);
            break;
    }
    return DefSubclassProc(h, m, w, l);
}

/* owner-drawn button rendering */
static void DrawCustomButton(const DRAWITEMSTRUCT* dis) {
    const BTNDEF* def = GetBtnDef((int)dis->CtlID);
    BTNSTATE* st = GetBtnState((int)dis->CtlID);
    const wchar_t* text;
    HFONT font;
    COLORREF fill;
    RECT rcT;

    if (!def || !st) return;

    text = def->text;
    font = def->bold ? g_hFontBtnBold : g_hFontBtn;
    fill = def->base;

    if (dis->CtlID == ID_BTN_SMALLSTICKY) {
        text = g_smallWinRun ? L"持续保持小窗: 开" : L"持续保持小窗: 关";
        fill = g_smallWinRun ? UI_SUCCESS : UI_GRAY_BTN;
    } else if (dis->CtlID == ID_BTN_SCREENGUARD) {
        text = g_screenGuard ? L"屏蔽屏幕监控: 开  (老师端只看到定格画面)"
                             : L"屏蔽屏幕监控: 关";
        fill = g_screenGuard ? UI_SUCCESS : UI_GRAY_BTN;
    } else {
        if (dis->itemState & ODS_SELECTED)      fill = def->dn;
        else if (st->hover)                     fill = def->hv;
    }

    FillRoundedRect(dis->hDC, dis->rcItem, BTN_RADIUS, fill);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, UI_TEXT);
    SelectObject(dis->hDC, font);
    rcT = dis->rcItem; /* DrawTextW needs a writable RECT */
    DrawTextW(dis->hDC, text, -1, &rcT,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

/* custom dark title bar with min/close buttons */
static void DrawTitleBar(HWND hwnd, HDC hdc) {
    RECT rc;
    int w, mid;
    HBRUSH ob;
    HPEN op, pen;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    mid = g_titleH / 2;

    /* background + separator */
    rc.left = 0; rc.top = 0; rc.right = w; rc.bottom = g_titleH;
    FillRect(hdc, &rc, g_hbrTitleBg);
    rc.top = g_titleH - 1; rc.bottom = g_titleH;
    FillRect(hdc, &rc, g_hbrBorder);

    /* accent dot */
    {
        HBRUSH dotBr = CreateSolidBrush(UI_ACCENT);
        ob = (HBRUSH)SelectObject(hdc, dotBr);
        op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, S(18), mid - S(5), S(28), mid + S(5));
        SelectObject(hdc, ob);
        SelectObject(hdc, op);
        DeleteObject(dotBr);           /* 以前漏删: 每次 WM_PAINT 漏一个 GDI 句柄 */
    }

    /* title text */
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, UI_TEXT);
    SelectObject(hdc, g_hFontTitle);
    rc.left = S(38); rc.top = 0; rc.right = w - S(100); rc.bottom = g_titleH;
    DrawTextW(hdc, WINDOW_TITLE, -1, &rc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    /* hover background for min / close */
    if (g_hoverTitle == 1) {
        RECT rm = g_rcTitleMin;
        FillRect(hdc, &rm, g_hbrMinHv);
    } else if (g_hoverTitle == 2) {
        RECT rx = g_rcTitleClose;
        FillRect(hdc, &rx, g_hbrCloseHv);
    }

    /* glyphs: minimize line + close cross */
    pen = CreatePen(PS_SOLID, 1,
        (g_hoverTitle == 2) ? RGB(255, 255, 255) : UI_TEXT);
    op = (HPEN)SelectObject(hdc, pen);
    MoveToEx(hdc, g_rcTitleMin.left + S(18), mid, NULL);
    LineTo(hdc, g_rcTitleMin.left + S(28), mid);
    MoveToEx(hdc, g_rcTitleClose.left + S(18), mid - S(5), NULL);
    LineTo(hdc, g_rcTitleClose.left + S(28), mid + S(5));
    MoveToEx(hdc, g_rcTitleClose.left + S(28), mid - S(5), NULL);
    LineTo(hdc, g_rcTitleClose.left + S(18), mid + S(5));
    SelectObject(hdc, op);
    DeleteObject(pen);
}

/* responsive layout (called on WM_SIZE / DPI change) */
static void LayoutControls(HWND hwnd) {
    RECT rc;
    int w, h, logTop, logBottom, by;
    HWND hCtl;

    GetClientRect(hwnd, &rc);
    w = rc.right;
    h = rc.bottom;

    g_titleH = S(TITLEBAR_H);
    SetRect(&g_rcTitleMin,   w - S(92), 0, w - S(46), g_titleH);
    SetRect(&g_rcTitleClose, w - S(46), 0, w,         g_titleH);

    /* 功能按钮 + 勾选框也在这里统一摆位:
       ★ 以前它们只在 CreateControls 里按当时的 DPI 建一次, 换到别的显示器
         (WM_DPICHANGED) 后字号变大、按钮还是旧尺寸, 文字就被裁掉了。 */
    {
        static const int ids[4] = { ID_BTN_UNLOCK_KB, ID_BTN_KILL_PROC,
                                    ID_BTN_UNINSTALL, ID_BTN_SMALLSTICKY };
        int colX[2], i;

        colX[0] = S(UI_PAD);
        colX[1] = S(UI_PAD) + S(BTN2_W) + S(BTN_GAP);
        for (i = 0; i < 4; i++) {
            HWND b = GetDlgItem(hwnd, ids[i]);
            if (b)
                MoveWindow(b, colX[i & 1], S(i < 2 ? ROW1_Y : ROW2_Y),
                           S(BTN2_W), S(BTN_H), TRUE);
        }
        hCtl = GetDlgItem(hwnd, ID_CHK_TRAY);
        if (hCtl) MoveWindow(hCtl, S(UI_PAD), S(CHK_Y), S(300), S(24), TRUE);
        hCtl = GetDlgItem(hwnd, ID_CHK_SHUT);
        if (hCtl) MoveWindow(hCtl, S(UI_PAD + 308), S(CHK_Y), S(360), S(24), TRUE);
        hCtl = GetDlgItem(hwnd, ID_BTN_SCREENGUARD);
        if (hCtl) MoveWindow(hCtl, S(UI_PAD), S(ROW3_Y),
                             S(BTN2_W) * 2 + S(BTN_GAP), S(BTN_H), TRUE);
    }

    logTop    = S(LOG_Y);
    logBottom = h - S(56);
    if (g_hwndLog && (logBottom - logTop) > S(60)) {
        MoveWindow(g_hwndLog, S(UI_PAD + 2), logTop, w - 2 * S(UI_PAD) - 4,
                   logBottom - logTop, TRUE);
    }

    by = h - S(50);
    hCtl = GetDlgItem(hwnd, ID_BTN_CHECK_STATUS);
    if (hCtl) MoveWindow(hCtl, S(UI_PAD), by, S(110), S(34), TRUE);
    hCtl = GetDlgItem(hwnd, ID_BTN_EXIT);
    if (hCtl) MoveWindow(hCtl, S(UI_PAD + 120), by, S(90), S(34), TRUE);
    SetRect(&g_rcStatusArea, S(UI_PAD + 230), by, w - S(UI_PAD), by + S(34));
}

/* ========== 创建控件 ========== */

/* 只有这两个控件"持有"字体句柄 (owner-draw 按钮是绘制时现取的)。
   ★ 重建字体 (WM_DPICHANGED) 之后必须重新下发一次, 否则它们还指着一个
     已经 DeleteObject 掉的 HFONT —— 换到不同 DPI 的显示器后日志区/勾选框
     会画不出来或者用回默认字体。 */
static void ApplyControlFonts(HWND hwnd)
{
    HWND h;

    if (!hwnd) return;
    h = GetDlgItem(hwnd, ID_LOG_OUTPUT);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontLog, TRUE);
    h = GetDlgItem(hwnd, ID_CHK_TRAY);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontHint, TRUE);
    h = GetDlgItem(hwnd, ID_CHK_SHUT);
    if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFontHint, TRUE);
}

void CreateControls(HWND hwnd) {
    HINSTANCE hInstance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    int colX[2];
    int ids1[2], ids2[2];
    int i, y;
    HWND hBtn;

    CreateUIFonts();
    /* 两个宽按钮一列: 4 个功能按钮排成 2x2, 宽度占满整行 */
    colX[0] = S(UI_PAD);
    colX[1] = S(UI_PAD) + S(BTN2_W) + S(BTN_GAP);

    /* row 1: 即时响应 (解锁键盘 / 结束进程) */
    ids1[0] = ID_BTN_UNLOCK_KB;
    ids1[1] = ID_BTN_KILL_PROC;
    y = S(ROW1_Y);
    for (i = 0; i < 2; i++) {
        hBtn = CreateWindowW(L"BUTTON", L"",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            colX[i], y, S(BTN2_W), S(BTN_H),
            hwnd, (HMENU)(INT_PTR)ids1[i], hInstance, NULL);
        SetWindowSubclass(hBtn, BtnProc, (UINT_PTR)ids1[i], 0);
    }

    /* row 2: 持久化改动 / 小窗主开关 (持续保持小窗) */
    ids2[0] = ID_BTN_UNINSTALL;
    ids2[1] = ID_BTN_SMALLSTICKY;
    y = S(ROW2_Y);
    for (i = 0; i < 2; i++) {
        hBtn = CreateWindowW(L"BUTTON", L"",
            WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
            colX[i], y, S(BTN2_W), S(BTN_H),
            hwnd, (HMENU)(INT_PTR)ids2[i], hInstance, NULL);
        SetWindowSubclass(hBtn, BtnProc, (UINT_PTR)ids2[i], 0);
    }

    /* row 3: 屏幕监控守卫 (整行宽按钮) */
    hBtn = CreateWindowW(L"BUTTON", L"",
        WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
        S(UI_PAD), S(ROW3_Y), S(BTN2_W) * 2 + S(BTN_GAP), S(BTN_H),
        hwnd, (HMENU)(INT_PTR)ID_BTN_SCREENGUARD, hInstance, NULL);
    SetWindowSubclass(hBtn, BtnProc, (UINT_PTR)ID_BTN_SCREENGUARD, 0);

    /* log output (border painted by WM_PAINT) */
    g_hwndLog = CreateWindowW(L"EDIT", L"",
        WS_VISIBLE | WS_CHILD | WS_VSCROLL | WS_HSCROLL |
        ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY,
        S(UI_PAD + 2), S(LOG_Y), S(BTN2_W) * 2 + S(BTN_GAP) - S(4), S(326),
        hwnd, (HMENU)(INT_PTR)ID_LOG_OUTPUT, hInstance, NULL);

    /* 广播窗最小化行为: 任务栏(真最小化) / 收起(托盘) */
    CreateWindowW(L"BUTTON", L"广播窗最小化到任务栏",
        WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        S(UI_PAD), S(CHK_Y), S(300), S(24),
        hwnd, (HMENU)(INT_PTR)ID_CHK_TRAY, hInstance, NULL);
    CheckDlgButton(hwnd, ID_CHK_TRAY,
                   ReadMinimizeTaskbarMode() ? BST_CHECKED : BST_UNCHECKED);

    /* 远程重启/关机守卫: 只吞控制端的 0x27/0x28, 不影响本机自己关机 */
    CreateWindowW(L"BUTTON", L"屏蔽控制端重启/关机",
        WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
        S(UI_PAD + 308), S(CHK_Y), S(360), S(24),
        hwnd, (HMENU)(INT_PTR)ID_CHK_SHUT, hInstance, NULL);
    CheckDlgButton(hwnd, ID_CHK_SHUT, g_shutGuard ? BST_CHECKED : BST_UNCHECKED);

    /* bottom row buttons (positioned by LayoutControls) */
    hBtn = CreateWindowW(L"BUTTON", L"",
        WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
        S(UI_PAD), S(642), S(110), S(34),
        hwnd, (HMENU)(INT_PTR)ID_BTN_CHECK_STATUS, hInstance, NULL);
    SetWindowSubclass(hBtn, BtnProc, (UINT_PTR)ID_BTN_CHECK_STATUS, 0);

    hBtn = CreateWindowW(L"BUTTON", L"",
        WS_VISIBLE | WS_CHILD | BS_OWNERDRAW,
        S(UI_PAD + 120), S(642), S(90), S(34),
        hwnd, (HMENU)(INT_PTR)ID_BTN_EXIT, hInstance, NULL);
    SetWindowSubclass(hBtn, BtnProc, (UINT_PTR)ID_BTN_EXIT, 0);

    ApplyControlFonts(hwnd);
    LayoutControls(hwnd);
}

/* ============================================================================
 *  按钮点击处理
 *
 *  这一节的每个 On* 函数 = g_btnDefs[] 里的一个按钮 = WM_COMMAND 里的一个分支。
 *  命名对照:
 *    解锁键盘          -> OnUnlockKeyboard
 *    结束课堂进程      -> OnKillProcesses
 *    卸载过滤驱动      -> OnUninstallDriver
 *    检查当前状态      -> OnCheckStatus
 *    退出              -> SC_CLOSE
 *    持续保持小窗      -> OnSmallStickyToggle (开关)
 * ========================================================================== */

/* 「解锁键盘」: 发实证解锁码 0xB2008, 优先原生 IOCTL, 失败退回厂商 DLL */
void OnUnlockKeyboard(HWND hwnd) {
    UnlockKeyboard(hwnd);
}


/* 「结束课堂进程」: 看守进程优先, 再杀主体与广播链 */
/* 后台执行"结束课堂进程": AppendLog 对非 UI 线程走 PostMessage,
   UpdateStatus 只做 InvalidateRect + 托盘提示, 都是线程安全的。 */
static volatile LONG g_killBusy = 0;

static DWORD WINAPI KillProcThread(LPVOID lp)
{
    KillControlProcesses((HWND)lp);
    InterlockedExchange(&g_killBusy, 0);
    return 0;
}

void OnKillProcesses(HWND hwnd) {
    /* ★ 不能在 UI 线程上同步跑: 每个目标最多等 1 秒, 13 个目标 + 驱动卡死时
       就是几十秒的"假死"。放后台线程, 日志照常流回编辑框。 */
    HANDLE th;

    if (InterlockedCompareExchange(&g_killBusy, 1, 0) != 0) {
        AppendLog(hwnd, "[Process] termination already in progress\r\n");
        return;
    }
    th = CreateThread(NULL, 0, KillProcThread, hwnd, 0, NULL);
    if (th) {
        CloseHandle(th);
        return;
    }
    InterlockedExchange(&g_killBusy, 0);
    KillControlProcesses(hwnd);      /* 线程建不起来就退回原来的同步路径 */
}

/* 「卸载过滤驱动」: 停删驱动服务 + 清键盘类 UpperFilters */
void OnUninstallDriver(HWND hwnd) {
    UninstallDriver(hwnd);
}


/* 「检查当前状态」: 清日志后做一次完整只读诊断 */
void OnCheckStatus(HWND hwnd) {
    ClearLog(hwnd);
    CheckStatus(hwnd);
}

/* ================= 配置 + 共享状态 =================
 *
 * 持久设置: bin\bypass_config.ini（人工可读，原子替换）
 * 运行期状态/一次性命令: Local\BypassTool_State_v1 + 事件
 * 安全阀: bypass_hook.off 仍是独立文件，不进这份状态。
 */
static HANDLE        g_stateMap;
static HANDLE        g_stateEvent;
static BypassState*  g_stateView;
static BypassState   g_state;
static CRITICAL_SECTION g_stateCs;
static int           g_stateCsReady;
static char          g_stateConfig[MAX_PATH];
static DWORD         g_stateSelfWriteTick;

static void StateDefaults(BypassState* s)
{
    memset(s, 0, sizeof(*s));
    s->magic = BYPASS_STATE_MAGIC;
    s->version = BYPASS_STATE_VERSION;
    s->size = (unsigned int)sizeof(BypassState);
    s->window_mode = 1;
    s->title_autohide = 1;
    s->minimize_tray = 0;
}

static void StateConfigPath(void)
{
    char dir[MAX_PATH];
    g_stateConfig[0] = '\0';
    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    PathRemoveFileSpecA(dir);
    PathCombineA(g_stateConfig, dir, BYPASS_CONFIG_FILE);
}

static void StateLock(void)
{
    if (g_stateCsReady) EnterCriticalSection(&g_stateCs);
}

static void StateUnlock(void)
{
    if (g_stateCsReady) LeaveCriticalSection(&g_stateCs);
}

static char* StateTrim(char* s)
{
    char* e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t'))
        *--e = '\0';
    return s;
}

/* "0.35" -> permille=350; "640x360" -> w/h; 空/0 -> 默认 1/3 屏宽 */
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

/* 只在配置缺省时读一次旧文件，之后不再使用这些文件。 */
static void StateMigrateLegacyLocked(void)
{
    char dir[MAX_PATH], p[MAX_PATH];
    char buf[64];
    FILE* f;

    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    PathRemoveFileSpecA(dir);

    PathCombineA(p, dir, "bypass_window_mode.off");
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) g_state.window_mode = 0;

    PathCombineA(p, dir, "bypass_title_autohide.off");
    if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) g_state.title_autohide = 0;

    PathCombineA(p, dir, "bypass_minimize_mode.txt");
    f = fopen(p, "r");
    if (f) {
        buf[0] = '\0';
        if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
        fclose(f);
        if (strstr(buf, "tray")) g_state.minimize_tray = 1;
    }

    PathCombineA(p, dir, "bypass_small.txt");
    f = fopen(p, "r");
    if (f) {
        buf[0] = '\0';
        if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
        fclose(f);
        StateParseSmall(buf, &g_state.small_w, &g_state.small_h, &g_state.small_permille);
    }
}

static void StateLoadConfigLocked(void)
{
    FILE* f;
    char line[256];
    unsigned int show = g_state.show_seq;
    unsigned int small = g_state.small_seq;

    f = fopen(g_stateConfig, "r");
    if (!f) return;

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
            g_state.window_mode = atoi(value) ? 1 : 0;
        } else if (!_stricmp(key, "title_autohide")) {
            g_state.title_autohide = atoi(value) ? 1 : 0;
        } else if (!_stricmp(key, "minimize_mode")) {
            g_state.minimize_tray = (strstr(value, "tray") != NULL) ? 1 : 0;
        } else if (!_stricmp(key, "small")) {
            StateParseSmall(value, &g_state.small_w, &g_state.small_h,
                            &g_state.small_permille);
        }
    }
    fclose(f);

    g_state.show_seq = show;
    g_state.small_seq = small;
}

static void StateSaveConfigLocked(void)
{
    FILE* f;
    char tmp[MAX_PATH];
    char small[64];

    if (!g_stateConfig[0]) return;
    _snprintf(tmp, sizeof(tmp) - 1, "%s.tmp", g_stateConfig);
    tmp[sizeof(tmp) - 1] = '\0';

    if (g_state.small_w > 0 && g_state.small_h > 0)
        _snprintf(small, sizeof(small) - 1, "%dx%d", g_state.small_w, g_state.small_h);
    else if (g_state.small_permille > 0)
        _snprintf(small, sizeof(small) - 1, "0.%03d", g_state.small_permille);
    else
        small[0] = '\0';
    small[sizeof(small) - 1] = '\0';

    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# bypass_tool runtime settings\n");
    fprintf(f, "window_mode=%d\n", g_state.window_mode ? 1 : 0);
    fprintf(f, "title_autohide=%d\n", g_state.title_autohide ? 1 : 0);
    fprintf(f, "minimize_mode=%s\n", g_state.minimize_tray ? "tray" : "taskbar");
    fprintf(f, "small=%s\n", small);
    fclose(f);

    if (!MoveFileExA(tmp, g_stateConfig,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileA(tmp);
    }
    g_stateSelfWriteTick = GetTickCount();
}

/* seqlock 发布: 奇数 seq 表示写入中，DLL 读到奇数会重试。 */
static void StatePublishLocked(void)
{
    size_t off;

    if (!g_stateView) return;
    InterlockedIncrement(&g_stateView->seq);
    MemoryBarrier();
    off = offsetof(BypassState, window_mode);
    memcpy((char*)g_stateView + off, (const char*)&g_state + off,
           sizeof(BypassState) - off);
    MemoryBarrier();
    InterlockedIncrement(&g_stateView->seq);
    if (g_stateEvent) SetEvent(g_stateEvent);
}

static BOOL StateMinimizeTaskbar(void)
{
    return g_state.minimize_tray ? FALSE : TRUE;
}

static void StateSetMinimizeTaskbar(BOOL taskbar)
{
    StateLock();
    g_state.minimize_tray = taskbar ? 0 : 1;
    StateSaveConfigLocked();
    StatePublishLocked();
    StateUnlock();
}

/* 配置文件被人工修改时，1 秒内重新加载并发布（跳过自己刚写的 1.5 秒）。 */
static DWORD WINAPI StateConfigWatch(LPVOID lp)
{
    FILETIME last;
    int first = 1;

    (void)lp;
    memset(&last, 0, sizeof(last));
    for (;;) {
        HANDLE f;
        FILETIME ft;

        Sleep(1000);
        if (GetTickCount() - g_stateSelfWriteTick < 1500) continue;

        f = CreateFileA(g_stateConfig, GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            first = 1;
            continue;
        }
        if (!GetFileTime(f, NULL, NULL, &ft)) {
            CloseHandle(f);
            continue;
        }
        CloseHandle(f);

        if (!first &&
            ft.dwLowDateTime == last.dwLowDateTime &&
            ft.dwHighDateTime == last.dwHighDateTime)
            continue;

        first = 0;
        last = ft;
        StateLock();
        StateLoadConfigLocked();
        g_state.config_seq++;
        StatePublishLocked();
        StateUnlock();
    }
    return 0;
}

static void StateInit(void)
{
    InitializeCriticalSection(&g_stateCs);
    g_stateCsReady = 1;
    StateConfigPath();
    StateDefaults(&g_state);
    StateLoadConfigLocked();

    if (g_stateConfig[0] &&
        GetFileAttributesA(g_stateConfig) == INVALID_FILE_ATTRIBUTES) {
        StateMigrateLegacyLocked();
        StateSaveConfigLocked();
    }

    g_stateMap = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                    0, sizeof(BypassState), BYPASS_STATE_MAP_NAME);
    if (g_stateMap) {
        g_stateView = (BypassState*)MapViewOfFile(g_stateMap, FILE_MAP_WRITE,
                                                  0, 0, sizeof(BypassState));
        if (g_stateView) {
            g_stateView->magic = BYPASS_STATE_MAGIC;
            g_stateView->version = BYPASS_STATE_VERSION;
            g_stateView->size = (unsigned int)sizeof(BypassState);
            g_stateView->seq = 0;
        }
    }
    g_stateEvent = CreateEventA(NULL, FALSE, FALSE, BYPASS_STATE_EVT_NAME);

    StateLock();
    StatePublishLocked();
    StateUnlock();

    CreateThread(NULL, 0, StateConfigWatch, NULL, 0, NULL);
}

/* 兼容原调用点 */
static BOOL ReadMinimizeTaskbarMode(void)
{
    return StateMinimizeTaskbar();
}

static void WriteMinimizeMode(BOOL taskbar)
{
    StateSetMinimizeTaskbar(taskbar);
}

/* 「持续保持小窗」开关: 开 = WinEvent 钩子即时小窗化 + 轮询兜底; 关 = 卸载钩子 */
void OnSmallStickyToggle(HWND hwnd) {
    if (g_smallWinRun) {
        StopSmallWin(hwnd);
        /* 用户主动关掉"持续保持" = 要求放行: 写 bypass_hook.off,
           已注入的 DLL 2 秒内停止干预窗口 (渲染进程不用重启)。 */
        SetHookOffFlag(TRUE);
        AppendLog(hwnd,
            "[Hook] 已写 bypass_hook.off: 已注入的 DLL 停止干预窗口 "
            "(渲染进程不用重启; 想重新接管请再点一次「持续保持小窗」)\r\n");
    } else {
        StartSmallWin(hwnd);
    }
}

/* ===== 屏幕监控守卫 =====
   老师端的"屏幕监控/录屏"在学生端走 GDI (REA/Ghidra 实证 StudentLogic.dll
   FUN_10025610: CreateDCW("DISPLAY") -> BitBlt -> GetDIBits)。注入的 DLL
   钩住 BitBlt: 第一次抓屏存一帧当"定格帧", 之后每次抓屏都从缓存画出来,
   屏幕一个像素都不再读。这里只负责开关 (写 ini) 和保证 DLL 已注入。 */
static void ScreenGuardPath(char* out, DWORD size)
{
    char dir[MAX_PATH];

    if (!out || size < 2) return;
    out[0] = '\0';
    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    PathRemoveFileSpecA(dir);
    PathCombineA(out, dir, "bypass_screen.ini");
}

static void ScreenGuardLoad(void)
{
    char  path[MAX_PATH];
    char  line[160];
    FILE* f;

    g_screenGuard = FALSE;
    ScreenGuardPath(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        char* v;

        if (!eq) continue;
        *eq = '\0';
        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!_stricmp(line, "enabled"))
            g_screenGuard = (atoi(v) != 0) ? TRUE : FALSE;
    }
    fclose(f);
}

static void ScreenGuardSave(void)
{
    char  path[MAX_PATH];
    char  tmp[MAX_PATH];
    FILE* f;

    ScreenGuardPath(path, sizeof(path));
    if (!path[0]) return;
    _snprintf(tmp, sizeof(tmp) - 1, "%s.tmp", path);
    tmp[sizeof(tmp) - 1] = '\0';

    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "; 屏幕监控守卫: 注入的 bypass_hook32.dll 读取本文件 (0.5 秒内生效)\r\n");
    fprintf(f, ";   enabled=1    老师端只能看到\"定格画面\", 不传实时桌面\r\n");
    fprintf(f, ";   mode=freeze  第一次抓屏作为定格帧 (默认) / blank = 固定纯色\r\n");
    fprintf(f, "enabled=%d\r\n", g_screenGuard ? 1 : 0);
    fprintf(f, "mode=freeze\r\n");
    fclose(f);

    /* 原子替换: DLL 每 0.5 秒读一次, 不能让它读到写了一半的文件 */
    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileA(tmp);
}

static void OnScreenGuardToggle(HWND hwnd)
{
    g_screenGuard = g_screenGuard ? FALSE : TRUE;
    ScreenGuardSave();

    /* 守卫逻辑在注入的 DLL 里: 打开时确保它已经进了 Student.exe 等进程 */
    if (g_screenGuard) RunInjector(hwnd, TRUE);

    /* ⚠ 中文只能写在**格式串**里: AppendLog 只把格式串从 UTF-8 转成 ACP,
       而 %s 的实参按 ACP 处理 —— 直接把 "开"/"关" 当参数传会变成
       "寮€/鍏?" 这种乱码 (UTF-8 字节被当成 GBK 显示)。 */
    if (g_screenGuard) {
        AppendLog(hwnd, "\r\n[Screen] 屏幕监控守卫 -> 开 (bin\\bypass_screen.ini)\r\n"
                        "[Screen] 第一次抓屏存成定格帧, 之后老师端只看到这张静止画面\r\n");
    } else {
        AppendLog(hwnd, "\r\n[Screen] 屏幕监控守卫 -> 关 (bin\\bypass_screen.ini)\r\n"
                        "[Screen] 恢复正常上传\r\n");
    }
    UpdateStatus(hwnd, g_screenGuard ? "Status: screen guard ON (frozen frame)"
                                     : "Status: screen guard OFF");
    {
        HWND b = GetDlgItem(hwnd, ID_BTN_SCREENGUARD);
        if (b) InvalidateRect(b, NULL, TRUE);
    }
}

/* ===== 远程重启/关机守卫 =====
   老师端的「远程关机 / 远程重启」在协议里就是两个命令字节 (REA/Ghidra 实证
   Student.exe FUN_00413560: 0x27 重启, 0x28 关机)。学生端自己的执行路径:
     0x27 -> ExitWindowsEx(EWX_REBOOT|EWX_FORCE)
     0x28 -> systemoper.dll!SYS_RunCommand("shutdown -s -t 1")
   注入的 DLL 在函数入口把这两条路吞掉 (返回"成功"避免控制端重试)。
   这里只负责开关: 写 bin\bypass_shut.ini, DLL 0.5 秒内读到。 */
static void ShutGuardPath(char* out, DWORD size)
{
    char dir[MAX_PATH];

    if (!out || size < 2) return;
    out[0] = '\0';
    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    PathRemoveFileSpecA(dir);
    PathCombineA(out, dir, "bypass_shut.ini");
}

static void ShutGuardLoad(void)
{
    char  path[MAX_PATH];
    char  line[160];
    FILE* f;

    g_shutGuard = FALSE;
    ShutGuardPath(path, sizeof(path));
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        char* v;

        if (!eq) continue;
        *eq = '\0';
        v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (!_stricmp(line, "enabled"))
            g_shutGuard = (atoi(v) != 0) ? TRUE : FALSE;
    }
    fclose(f);
}

static void ShutGuardSave(void)
{
    char  path[MAX_PATH];
    char  tmp[MAX_PATH];
    FILE* f;

    ShutGuardPath(path, sizeof(path));
    if (!path[0]) return;
    _snprintf(tmp, sizeof(tmp) - 1, "%s.tmp", path);
    tmp[sizeof(tmp) - 1] = '\0';

    f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "; 远程重启/关机守卫: 注入的 bypass_hook32.dll 读取本文件 (0.5 秒内生效)\r\n");
    fprintf(f, ";   enabled=1    忽略老师端的 0x27(重启) / 0x28(关机), 只写日志\r\n");
    fprintf(f, ";   enabled=0    正常: 控制端可以远程关机/重启本机 (默认)\r\n");
    fprintf(f, "enabled=%d\r\n", g_shutGuard ? 1 : 0);
    fclose(f);

    if (!MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        DeleteFileA(tmp);
}

/* 统一的开关入口: 主界面勾选框和托盘菜单都走这里, 保证 ini / DLL / 控件
   三者状态永远一致。 */
static void SetShutGuard(HWND hwnd, BOOL on)
{
    g_shutGuard = on ? TRUE : FALSE;
    ShutGuardSave();

    /* 守卫逻辑在注入的 DLL 里: 打开时确保它已经进了 Student.exe 等进程 */
    if (g_shutGuard) RunInjector(hwnd, TRUE);

    /* 同步主界面勾选框 (托盘菜单打开时窗口可能正收在托盘里) */
    if (hwnd && IsWindow(hwnd)) {
        CheckDlgButton(hwnd, ID_CHK_SHUT, g_shutGuard ? BST_CHECKED : BST_UNCHECKED);
        InvalidateRect(hwnd, NULL, FALSE);
    }

    if (g_shutGuard) {
        AppendLog(hwnd, "\r\n[Guard] 屏蔽控制端重启/关机 -> 开 (bin\\bypass_shut.ini)\r\n"
                        "[Guard] 老师端的 0x27(重启)/0x28(关机) 会被吞掉, 只记一行日志\r\n");
    } else {
        AppendLog(hwnd, "\r\n[Guard] 屏蔽控制端重启/关机 -> 关 (bin\\bypass_shut.ini)\r\n"
                        "[Guard] 恢复正常: 老师端可以远程关机/重启本机\r\n");
    }
    UpdateStatus(hwnd, g_shutGuard ? "Status: remote shutdown blocked"
                                   : "Status: remote shutdown allowed");
}

static void OnShutGuardToggle(HWND hwnd)
{
    SetShutGuard(hwnd,
                 (IsDlgButtonChecked(hwnd, ID_CHK_SHUT) == BST_CHECKED) ? TRUE : FALSE);
}

/* ========== 托盘 (notification area) ==========
   设计目标:
     - 窗口可见时任务栏有按钮 (WS_EX_APPWINDOW), 收进托盘后按钮随 SW_HIDE 消失;
     - 托盘图标是最快入口: 左键单击/双击 = 显示/隐藏主界面, 右键 = 菜单;
     - 菜单里保留最常用的三个动作 (解锁键盘 / 恢复全屏 / 应急点击), 不用先开窗口.
   ⚠ Shell_NotifyIcon 需要窗口仍然存在 —— 关闭窗口时只 Hide, 不 Destroy. */

/* ---- 单一日志文件 (bin\bypass_hook.log) 的独立写入器 ------------------------
   GUI 模式下 AppendLog 只写编辑框 (g_logFile 仅静默模式打开), 但托盘这种
   "看不到就找不回来"的事件必须能从 bin\bypass_hook.log 里查到。这里用
   FILE_APPEND_DATA 追加, 与 elevate.c 的 ELog、注入 DLL 共用同一个文件,
   多进程并发写不会互相覆盖。写进去的内容保持纯 ASCII。 */
static void FileLog(const char* fmt, ...)
{
    char       line[1024];
    char       dir[MAX_PATH], path[MAX_PATH];
    char*      p;
    SYSTEMTIME st;
    va_list    ap;
    int        n;
    HANDLE     h;
    DWORD      written = 0;

    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    p = strrchr(dir, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(path, sizeof(path) - 1, "%sbypass_hook.log", dir);
    path[sizeof(path) - 1] = '\0';

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

    h = CreateFileA(path, FILE_APPEND_DATA,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    WriteFile(h, line, (DWORD)n, &written, NULL);
    CloseHandle(h);
}

/* 托盘故障现场: 进程在哪个会话/窗口站/桌面、什么完整性级别、能不能看到
   shell 的 Shell_TrayWnd。运行在别的桌面/会话里时 Shell_NotifyIcon 会
   返回成功, 但图标永远不会出现在用户的托盘上 —— 这行日志能直接区分
   "图标创造失败" 和 "图标不存在于用户桌面"。 */
static void LogTrayEnvironment(HWND hwnd)
{
    DWORD   session = 0;
    DWORD   il = 0;
    HANDLE  tok = NULL;
    HWND    tray;
    HWINSTA wsH = GetProcessWindowStation();
    HDESK   deskH = GetThreadDesktop(GetCurrentThreadId());
    wchar_t userW[128] = L"?";
    wchar_t wsW[64] = L"?";
    wchar_t deskW[64] = L"?";
    char    user[128] = "?", ws[64] = "?", desk[64] = "?";
    DWORD   len;

    len = 128;
    GetUserNameW(userW, &len);
    WideCharToMultiByte(CP_ACP, 0, userW, -1, user, sizeof(user), NULL, NULL);

    len = 0;
    if (wsH)
        GetUserObjectInformationW(wsH, UOI_NAME, wsW, sizeof(wsW), &len);
    len = 0;
    if (deskH)
        GetUserObjectInformationW(deskH, UOI_NAME, deskW, sizeof(deskW), &len);
    WideCharToMultiByte(CP_ACP, 0, wsW, -1, ws, sizeof(ws), NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, deskW, -1, desk, sizeof(desk), NULL, NULL);

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD cb = 0;
        if (!GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &cb) &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER && cb) {
            TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)malloc(cb);
            if (tml) {
                if (GetTokenInformation(tok, TokenIntegrityLevel, tml, cb, &cb) &&
                    tml->Label.Sid) {
                    BYTE* cnt = GetSidSubAuthorityCount(tml->Label.Sid);
                    if (cnt && *cnt) {
                        DWORD* rid = GetSidSubAuthority(tml->Label.Sid,
                                                        (DWORD)(*cnt - 1));
                        if (rid) il = *rid;
                    }
                }
                free(tml);
            }
        }
        CloseHandle(tok);
    }

    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) session = 0;
    tray = FindWindowW(L"Shell_TrayWnd", NULL);

    FileLog("[Tray] env hwnd=%p session=%lu user=%s wsp=%s desktop=%s il=0x%lx "
            "tray_hwnd=%p",
            (void*)hwnd, (unsigned long)session, user, ws, desk,
            (unsigned long)il, (void*)tray);
}

static void TrayAdd(HWND hwnd) {
    NOTIFYICONDATAW nid;
    DWORD           err;

    if (g_trayAdded) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g_trayIcon;
    wcscpy(nid.szTip, L"教学控制解除工具 —— 双击打开");

    SetLastError(0);
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        g_trayAdded = TRUE;
        g_trayRetries = 0;
        /* Win7+ 才支持 NIM_SETVERSION, 失败也无所谓 */
        nid.uVersion = NOTIFYICON_VERSION_4;
        if (!Shell_NotifyIconW(NIM_SETVERSION, &nid))
            FileLog("[Tray] NIM_SETVERSION failed err=%lu (cosmetic)",
                    (unsigned long)GetLastError());
        FileLog("[Tray] icon added (icon=%p)", (void*)g_trayIcon);
        return;
    }

    /* 失败不要静默: 一次性重试 + 写日志, 否则表现就是"窗口不见了,
       托盘里也没有图标", 完全没有线索。 */
    err = GetLastError();
    g_trayRetries++;
    FileLog("[Tray] NIM_ADD failed err=%lu (attempt %d/%d, icon=%p)",
            (unsigned long)err, g_trayRetries, TRAY_RETRY_MAX,
            (void*)g_trayIcon);
    AppendLog(hwnd, "[Tray] 托盘图标创建失败 (err=%lu)，正在重试…\r\n",
              (unsigned long)err);

    /* 登录后立即启动 / Explorer 正在重启时, shell 的托盘可能还没准备好 */
    if (g_trayRetries <= TRAY_RETRY_MAX)
        SetTimer(hwnd, IDT_TRAY_RETRY, TRAY_RETRY_MS, NULL);
}

static void TrayRemove(HWND hwnd) {
    NOTIFYICONDATAW nid;

    if (!g_trayAdded) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = TRAY_ICON_ID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = FALSE;
    FileLog("[Tray] icon removed");
}

static void TrayUpdateTip(HWND hwnd, const wchar_t* tip) {
    NOTIFYICONDATAW nid;
    HWND h = hwnd ? hwnd : g_hwndMain;

    if (!g_trayAdded || !tip || !h) return;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = h;
    nid.uID = TRAY_ICON_ID;
    nid.uFlags = NIF_TIP;
    wcsncpy(nid.szTip, tip, 127);
    nid.szTip[127] = 0;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* 把主窗口叫出来:
   - 如果它被拖到屏幕外/另一块已拔掉的显示器上, 先挪回当前屏中央;
   - SW_SHOW + SW_RESTORE 处理"隐藏"和"最小化"两种情况;
   - 最后抢前台 (托盘点击时系统会给前台权限)。 */
static void ShowMainWindow(HWND hwnd) {
    RECT r;

    if (GetWindowRect(hwnd, &r)) {
        HMONITOR    mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi;
        int w = r.right - r.left;
        int h = r.bottom - r.top;

        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(mon, &mi) && w > 0 && h > 0) {
            RECT wa = mi.rcWork;
            int  offscreen =
                (r.right  < wa.left + 40) || (r.left > wa.right  - 40) ||
                (r.bottom < wa.top  + 40) || (r.top  > wa.bottom - 40);

            if (offscreen) {
                SetWindowPos(hwnd, NULL,
                             wa.left + (wa.right - wa.left - w) / 2,
                             wa.top  + (wa.bottom - wa.top - h) / 2,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    ShowWindow(hwnd, SW_RESTORE);
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
}

static void HideToTray(HWND hwnd) {
    NOTIFYICONDATAW nid;

    /* 没有托盘图标就绝不隐藏窗口: SW_HIDE 之后任务栏按钮也会跟着消失,
       Windows 又可能把图标折叠在 "^" 里, 用户就没有任何可见入口了
       (只剩 Ctrl+Alt+H 和再双击一次 exe)。宁可留在屏幕上。 */
    if (!g_trayAdded) TrayAdd(hwnd);
    if (!g_trayAdded) {
        FileLog("[Tray] hide refused: no tray icon, window stays visible");
        AppendLog(hwnd, "[Tray] 没有托盘图标，窗口保持显示（不隐藏）\r\n");
        return;
    }

    ShowWindow(hwnd, SW_HIDE);
    FileLog("[Tray] window hidden to tray");

    if (g_trayAdded && !g_trayBalloonShown) {
        g_trayBalloonShown = TRUE;
        memset(&nid, 0, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd;
        nid.uID = TRAY_ICON_ID;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_INFO;
        wcscpy(nid.szInfoTitle, L"仍在后台运行");
        wcscpy(nid.szInfo, L"单击托盘图标可以重新打开窗口。");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

static void TrayShowMenu(HWND hwnd) {
    HMENU   menu;
    POINT   pt;
    int     cmd;

    menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | MF_DEFAULT, IDM_TRAY_SHOW,
                L"显示主界面(&O)\tCtrl+Alt+H");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);

    /* 高频动作: 不打开窗口也能点。菜单里带上全局快捷键，省得记。 */
    AppendMenuW(menu, MF_STRING, IDM_TRAY_UNLOCK,     L"解锁键盘(&K)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_DESKTOP,    L"立即切回桌面(&D)\tCtrl+Alt+D");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_CLICK,      L"应急点击(&E)\tCtrl+Alt+E");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_NETDIAG,    L"网络/桌面诊断(&N)");
    AppendMenuW(menu, MF_STRING | (g_shutGuard ? MF_CHECKED : 0),
                IDM_TRAY_SHUT,                        L"屏蔽控制端重启/关机(&P)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_RELEASE,    L"恢复全屏广播（放行）(&R)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_ABOUT,      L"关于(&A)");
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT,       L"退出(&X)\tCtrl+Alt+Q");

    GetCursorPos(&pt);

    /* 让菜单能正常消失: 先置前台, 再 Track (经典做法) */
    SetForegroundWindow(hwnd);
    cmd = (int)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                              pt.x, pt.y, 0, hwnd, NULL);
    DestroyMenu(menu);

    /* 托盘菜单在某些 shell 下不会自动消失 */
    PostMessageW(hwnd, WM_NULL, 0, 0);

    if (cmd == 0) return;

    switch (cmd) {
    case IDM_TRAY_SHOW:
        ShowMainWindow(hwnd);
        break;
    case IDM_TRAY_UNLOCK:
        UnlockKeyboard(hwnd);
        break;
    case IDM_TRAY_DESKTOP: {
        char from[128] = "";
        if (RestoreDefaultDesktop(from, sizeof(from))) {
            AppendLog(hwnd, "\r\n[Tray] desktop restored (was '%s')\r\n", from[0] ? from : "?");
            UpdateStatus(hwnd, "Status: desktop restored");
        } else {
            AppendLog(hwnd, "\r\n[Tray] already on Default\r\n");
        }
        break;
    }
    case IDM_TRAY_CLICK:
        EmergencyClick();
        AppendLog(hwnd, "\r\n[Tray] emergency click sent (dwExtraInfo=0x12452)\r\n");
        break;
    case IDM_TRAY_NETDIAG:
        LogNetStatus(hwnd);
        break;
    case IDM_TRAY_SHUT:
        SetShutGuard(hwnd, g_shutGuard ? FALSE : TRUE);
        break;
    case IDM_TRAY_RELEASE:
        /* 放行: 停止监视 + 写 bypass_hook.off, 已注入的 DLL 2 秒内停止干预。
           下次启动本程序会自动清掉这个文件 (重新接管)。 */
        if (g_smallWinRun) StopSmallWin(hwnd);
        SetHookOffFlag(TRUE);
        AppendLog(hwnd,
            "\r\n[Hook] released: bypass_hook.off written, injected DLLs stand down\r\n"
            "[Hook] 教师端重新发起广播后即为全屏; 想再次小窗化请再点一次「持续保持小窗」\r\n");
        UpdateStatus(hwnd, "Status: released (fullscreen allowed)");
        /* 「持续保持小窗」按钮上的文字是现取的 (g_smallWinRun), 这里要重画 */
        InvalidateRect(hwnd, NULL, TRUE);
        break;
    case IDM_TRAY_ABOUT:
        ShowMainWindow(hwnd);
        AppendLog(hwnd,
            "\r\n[Info] 教学控制解除工具 v1.2\r\n"
            "[Info] 托盘: 左键单击=显示/隐藏, 右键=菜单, Ctrl+Alt+X=应急\r\n");
        break;
    case IDM_TRAY_EXIT:
        DestroyWindow(hwnd);
        break;
    default:
        break;
    }
}

/* ========== 窗口过程 ========== */

/* ==================================================================
 * 桌面守护 / 应急点击 / 诊断  (2026-10-22 逆向成果迁移)
 *
 * 背景: 学生端收到 0x1D 后, MultiClient 会 SwitchDesktop("NetTeachingDesktop")
 * 并拉起全屏渲染。如果控制连接断了(教师端异常/假教师退出), 学生端**不会**
 * 自己切回来 —— 那个桌面没有任务栏, 鼠标又被 MultiClient 的 WH_MOUSE_LL
 * 丢弃, 表现就是"彻底卡死, 只能重启"。
 *
 * 所以这里放一个独立看门狗: 只要检测到广播进程在跑, 且输入桌面不是 Default,
 * 就切回来。只切一次(3 秒去抖) —— MultiClient 不会循环重切, 不会闪。
 * ================================================================== */

static volatile LONG g_deskGuardStop    = 0;
static volatile LONG g_deskGuardEnabled = 1;
static DWORD         g_deskGuardLastAct = 0;
static DWORD         g_deskGuardLastUnlock = 0;   /* 解锁/日志节流 */
static int           g_deskGuardLogs    = 0;

static BOOL BroadcastRunning(void)
{
    return IsProcessRunning("MultiClient.exe") ||
           IsProcessRunning("ScreenRender.exe") ||
           IsProcessRunning("MultiRender.exe");
}

static BOOL RenderersRunning(void)
{
    return IsProcessRunning("ScreenRender.exe") ||
           IsProcessRunning("MultiRender.exe");
}

/* 只在"课堂自己的桌面"上动手, 避免和用户自己的虚拟桌面软件打架:
   - NetTeachingDesktop / MCLOCK Desktop: 一定是课堂切走的;
   - 其它非 Default 桌面: 只有渲染器确实在跑时才接管。 */
static BOOL ShouldRestoreDesktop(const char* name)
{
    if (!name || !name[0]) return FALSE;
    if (_stricmp(name, "Default") == 0) return FALSE;
    if (strstr(name, "NetTeaching") != NULL) return TRUE;
    if (_stricmp(name, "MCLOCK Desktop") == 0) return TRUE;
    return IsProcessRunning("ScreenRender.exe") ||
           IsProcessRunning("MultiRender.exe");
}

static BOOL SwitchToDesktop(const char* name)
{
    HDESK hd;
    BOOL  ok;

    hd = OpenDesktopA(name, 0, FALSE, DESKTOP_SWITCHDESKTOP);
    if (!hd) return FALSE;
    ok = SwitchDesktop(hd);
    CloseDesktop(hd);
    return ok;
}

/* 立即切回 Default (热键/托盘/按钮都走这里) */
static BOOL RestoreDefaultDesktop(char* fromOut, DWORD fromSize)
{
    char name[128];

    if (fromOut && fromSize) fromOut[0] = '\0';
    if (!GetInputDesktopName(name, sizeof(name))) return FALSE;
    if (fromOut && fromSize) {
        strncpy(fromOut, name, fromSize - 1);
        fromOut[fromSize - 1] = '\0';
    }
    if (_stricmp(name, "Default") == 0) return TRUE;
    return SwitchToDesktop("Default");
}

static DWORD WINAPI DesktopGuardProc(LPVOID lp)
{
    (void)lp;
    SafeLog("[DesktopGuard] watchdog started (1s poll, 3s debounce)\r\n");

    while (!InterlockedCompareExchange(&g_deskGuardStop, 0, 0)) {
        char  name[128];
        DWORD now = GetTickCount();

        /* ★ 只有"持续保持小窗"开启时才管桌面。
           用户没开小窗模式 = 要正常全屏广播, 看门狗绝不能去切桌面
           (否则会把广播画面藏到另一个桌面, 表现为"透明 + 鼠标点不动")。 */
        if (!g_smallWinRun) {
            Sleep(1000);
            continue;
        }

        if (InterlockedCompareExchange(&g_deskGuardEnabled, 0, 0) &&
            GetInputDesktopName(name, sizeof(name)) &&
            ShouldRestoreDesktop(name) &&
            (now - g_deskGuardLastAct) > 3000) {

            BOOL switched = SwitchToDesktop("Default");

            if (switched) g_deskGuardLastAct = now;

            if (switched && (now - g_deskGuardLastUnlock) > 3000) {
                g_deskGuardLastUnlock = now;
                if (g_deskGuardLogs < 20) {
                    g_deskGuardLogs++;
                    SafeLog("[DesktopGuard] input='%s' -> switched to Default "
                            "(broadcast process still running)\r\n", name);
                    /* 如果渲染器还在跑, 说明它有窗口在刚才那个桌面上:
                       切回来后画面可能看不见 —— 多半是 hook 没注入成功。 */
                    if (RenderersRunning()) {
                        SafeLog("[DesktopGuard] WARNING: renderer is still running; "
                                "if the video disappeared, the hook was not active "
                                "(check bypass_hook.log)\r\n");
                    }
                }
            }
        }
        Sleep(1000);
    }
    SafeLog("[DesktopGuard] watchdog stopped\r\n");
    return 0;
}

static void StartDesktopGuard(HWND hwnd)
{
    (void)hwnd;
    InterlockedExchange(&g_deskGuardStop, 0);
    if (!CreateThread(NULL, 0, DesktopGuardProc, NULL, 0, NULL))
        SafeLog("[DesktopGuard] ERROR: cannot start watchdog\r\n");
}

/* 默认开启「持续保持小窗」。
   ⚠ 必须在 UI 线程里启动: InstallBroadcastHook 用的是 WINEVENT_OUTOFCONTEXT,
   它的事件投递到"安装钩子的那个线程"的消息队列 —— 放到工作线程会收不到事件。
   代价: RunInjector 最长阻塞 8 秒 (通常 <1s), 启动瞬间界面可能短暂无响应。 */
#define WM_APP_AUTOSTART (WM_APP + 3)
/* "把自己叫到前台"：第二次双击 bypass.exe 时, 新实例（往往不是管理员）会找到
   本窗口并 PostMessage 这条消息。已提权的窗口默认拒收低完整性进程的消息,
   所以窗口创建后要用 ChangeWindowMessageFilterEx 明确放行。 */
#define WM_BYPASS_SHOW   (WM_APP + 4)
/* /auto: 把"一键绕过"投递到消息循环里跑 (它含 Sleep / 等驱动停止, 同步跑会让
   窗口在 ShowWindow 之后立刻"未响应")。 */
#define WM_APP_AUTOALL   (WM_APP + 5)

/* 应急点击: MultiClient 的 WH_MOUSE_LL 只放行 dwExtraInfo==0x12452 的事件,
   这里直接借用它自己的通行证 —— 鼠标被"冻结"时还能点一下。 */
static void EmergencyClick(void)
{
    INPUT in;

    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in.mi.dwExtraInfo = (ULONG_PTR)0x12452;
    SendInput(1, &in, sizeof(in));
    Sleep(40);
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in.mi.dwExtraInfo = (ULONG_PTR)0x12452;
    SendInput(1, &in, sizeof(in));
}

/* 诊断: 谁在连教师端 + 当前桌面 (只写日志, 不改任何状态) */
static void LogNetStatus(HWND hwnd)
{
    PMIB_TCPTABLE_OWNER_PID table = NULL;
    DWORD size = 0, i;
    char  dname[128];
    int   found = 0;

    AppendLog(hwnd, "\r\n[Net] ---- diagnostics ----\r\n");

    if (GetInputDesktopName(dname, sizeof(dname)))
        AppendLog(hwnd, "[Net] input desktop : %s\r\n", dname);
    else
        AppendLog(hwnd, "[Net] input desktop : (cannot query)\r\n");

    AppendLog(hwnd, "[Net] broadcast procs: MultiClient=%s ScreenRender=%s MultiRender=%s\r\n",
              IsProcessRunning("MultiClient.exe") ? "yes" : "no",
              IsProcessRunning("ScreenRender.exe") ? "yes" : "no",
              IsProcessRunning("MultiRender.exe") ? "yes" : "no");
    AppendLog(hwnd, "[Net] broadcast active: %s\r\n", BroadcastRunning() ? "yes" : "no");

    if (GetExtendedTcpTable(NULL, &size, FALSE, 2 /*AF_INET*/,
                            TCP_TABLE_OWNER_PID_ALL, 0) == ERROR_INSUFFICIENT_BUFFER &&
        size > 0) {
        table = (PMIB_TCPTABLE_OWNER_PID)malloc(size);
    }
    if (table &&
        GetExtendedTcpTable(table, &size, FALSE, 2 /*AF_INET*/,
                            TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
        for (i = 0; i < table->dwNumEntries; i++) {
            MIB_TCPROW_OWNER_PID* r = &table->table[i];
            DWORD pid = r->dwOwningPid;
            unsigned int rport = ((r->dwRemotePort & 0xFF) << 8) |
                                 ((r->dwRemotePort >> 8) & 0xFF);
            unsigned int lport = ((r->dwLocalPort & 0xFF) << 8) |
                                 ((r->dwLocalPort >> 8) & 0xFF);
            char  path[MAX_PATH], *base;
            char  lip[20], rip[20];

            if (r->dwState != MIB_TCP_STATE_ESTAB) continue;
            if (rport != 9003 && rport != 9002 && rport != 8003) continue;
            if (!GetProcessPath(pid, path, sizeof(path))) continue;
            base = strrchr(path, '\\');
            base = base ? base + 1 : path;

            /* 手工格式化: 避免再引入 winsock2.h (会和 windows.h 的 winsock.h 打架) */
            {
                unsigned char* b = (unsigned char*)&r->dwLocalAddr;
                _snprintf(lip, sizeof(lip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                b = (unsigned char*)&r->dwRemoteAddr;
                _snprintf(rip, sizeof(rip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            }
            AppendLog(hwnd, "[Net] %-18s pid=%-6lu %s:%u -> %s:%u\r\n",
                      base, (unsigned long)pid, lip, lport, rip, rport);
            found++;
        }
    }
    if (table) free(table);
    if (!found) AppendLog(hwnd, "[Net] no classroom control connection found\r\n");
    AppendLog(hwnd, "[Net] ---- end ----\r\n");
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    /* Explorer 重启 (或登录时 shell 尚未就绪) 会广播 TaskbarCreated:
       旧图标已经失效, 必须重新 NIM_ADD, 否则窗口一收进托盘就再也叫不出来 */
    if (g_msgTaskbarCreated && msg == g_msgTaskbarCreated) {
        FileLog("[Tray] TaskbarCreated -> re-adding icon");
        g_trayAdded = FALSE;
        g_trayRetries = 0;
        TrayAdd(hwnd);
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
            g_hwndMain = hwnd;
            CreateControls(hwnd);

            /* 全局热键: MultiClient.exe 的 WH_MOUSE_LL 会把物理鼠标事件全部丢弃,
               那时 GUI 点不动。留一个键盘逃生口 (键盘可由 KbFilter 解锁码恢复,
               鼠标钩子却只能靠结束 MultiClient.exe 解除)。
               另外给常用动作都配了 Ctrl+Alt+字母, 鼠标冻结时也能用。 */
            {
                struct { int id; UINT vk; const char* name; } hk[] = {
                    { ID_HOTKEY_ESCAPE,   'X', "Ctrl+Alt+X 应急 (结束课堂进程)" },
                    { ID_HOTKEY_PANEL,    'H', "Ctrl+Alt+H 显示/隐藏面板" },
                    { ID_HOTKEY_STICKY,   'T', "Ctrl+Alt+T 持续保持小窗" },
                    { ID_HOTKEY_DESKTOP,  'D', "Ctrl+Alt+D 立即切回桌面" },
                    { ID_HOTKEY_CLICK,    'E', "Ctrl+Alt+E 应急点击" },
                    { ID_HOTKEY_SCREEN,   'M', "Ctrl+Alt+M 屏蔽屏幕监控 (备用)" },
                    { ID_HOTKEY_QUIT,     'Q', "Ctrl+Alt+Q 退出" },
                };
                int i;
                for (i = 0; i < (int)(sizeof(hk) / sizeof(hk[0])); i++) {
                    if (!RegisterHotKey(hwnd, hk[i].id, MOD_CONTROL | MOD_ALT, hk[i].vk)) {
                        AppendLog(hwnd, "[Hotkey] WARNING: %s registration failed (err=%lu)\r\n",
                                  hk[i].name, GetLastError());
                    }
                }
            }

            AppendLog(hwnd, "Classroom Bypass Tool v1.2 initialized.\r\n");
            AppendLog(hwnd, "Hint: hover any button to see what it does (bottom-right).\r\n");
            AppendLog(hwnd, "Hotkeys (global, work even when the classroom tool grabs the mouse):\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+H  show / hide this panel\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+T  sticky small window (toggle)\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+D  restore desktop (if trapped on NetTeachingDesktop)\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+E  emergency click (passes MultiClient's mouse hook)\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+M  screen-monitor guard (frozen frame) toggle\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+Q  quit\r\n");
            AppendLog(hwnd, "  Ctrl+Alt+X  EMERGENCY: kill classroom processes\r\n");
            AppendLog(hwnd, "Click 'Check Status' for a read-only scan.\r\n");

            /* 托盘: 关闭窗口只收进托盘, 不退出 */
            g_trayIcon = (HICON)LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1),
                                           IMAGE_ICON, 16, 16, LR_SHARED);
            if (!g_trayIcon) g_trayIcon = LoadIconW(NULL, (LPCWSTR)IDI_SHIELD);
            /* Explorer 重启后会广播这条消息, 收到就得重新 NIM_ADD */
            g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
            LogTrayEnvironment(hwnd);
            TrayAdd(hwnd);

            /* 桌面看门狗: 教师端异常断线时, 学生端会永远留在教学桌面。
               看门狗每秒检查一次, 发现被切走就切回 Default。
               注意: 它只在「持续保持小窗」开启时才动手。 */
            StartDesktopGuard(hwnd);

            /* 默认开启「持续保持小窗」(投递到 UI 线程, 保证 WinEvent 钩子有消息循环) */
            PostMessage(hwnd, WM_APP_AUTOSTART, 0, 0);

            /* 上次"放行"留下的 bypass_hook.off 不能带到这次会话里:
               否则新注入的 DLL 一开始就被禁用, 表现为"点了没效果"。
               启动即视为"接管", 清掉它并说明。 */
            {
                char dir[MAX_PATH], flag[MAX_PATH];
                GetModuleFileNameA(NULL, dir, MAX_PATH);
                PathRemoveFileSpec(dir);
                PathCombine(flag, dir, "bypass_hook.off");
                if (GetFileAttributesA(flag) != INVALID_FILE_ATTRIBUTES) {
                    DeleteFileA(flag);
                    AppendLog(hwnd,
                        "[Hook] 已清除上次留下的 bypass_hook.off (重新接管)\r\n");
                }
            }
            return 0;

        /* 托盘图标回调 */
        case WM_TRAYICON:
            switch (LOWORD(lParam)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case NIN_SELECT:            /* NOTIFYICON_VERSION_4: 左键单击 */
            case NIN_KEYSELECT:         /* 键盘选中 */
                /* 点击托盘 = 打开界面。以前是 toggle: 窗口只是被别的窗口盖住时
                   IsWindowVisible 仍为真, 一点反而把窗口藏起来, 看起来像"点了没反应"。 */
                ShowMainWindow(hwnd);
                return 0;
            case WM_RBUTTONUP:
            case WM_CONTEXTMENU:        /* v4: 右键 */
                TrayShowMenu(hwnd);
                return 0;
            default:
                return 0;
            }

        /* 线程安全日志消息 */
        case WM_APP + 1: {
            char* text = (char*)lParam;
            if (text) {
                AppendLog(hwnd, "%s", text);
                free(text);
            }
            return 0;
        }

        /* 启动后自动进入「持续保持小窗」(默认开启) */
        case WM_APP_AUTOSTART:
            if (!g_smallWinRun) {
                AppendLog(hwnd,
                    "[Startup] sticky small-window is ON by default "
                    "(turn it off from the tray/button to allow fullscreen)\r\n");
                StartSmallWin(hwnd);
            }
            return 0;

        /* 第二个实例请求把自己显示出来（见 WinMain 里的早期单实例检查） */
        case WM_BYPASS_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return 0;

        /* /auto: 在后台线程跑 (AutoAllCore 里有 Sleep + 等驱动停止) */
        case WM_APP_AUTOALL: {
            HANDLE th;

            if (InterlockedCompareExchange(&g_autoAllBusy, 1, 0) != 0) return 0;
            AppendLog(hwnd, "[Startup] /auto: running the bypass in the background\r\n");
            th = CreateThread(NULL, 0, AutoAllThread, hwnd, 0, NULL);
            if (th) {
                CloseHandle(th);
            } else {
                InterlockedExchange(&g_autoAllBusy, 0);
                AutoAllCore(hwnd);      /* 线程建不起来就退回原来的同步路径 */
            }
            return 0;
        }

        /* full custom painting (dark theme) */
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc, r;
            COLORREF dot;
            HBRUSH b;
            HBRUSH ob;
            HPEN op;
            int cy;
            wchar_t wstat[256];

            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_hbrBg);
            DrawTitleBar(hwnd, hdc);

            SetBkMode(hdc, TRANSPARENT);

            /* section label: functions */
            SelectObject(hdc, g_hFontSection);
            SetTextColor(hdc, UI_TEXT_DIM);
            r.left = S(UI_PAD); r.top = S(52); r.right = S(400); r.bottom = S(72);
            DrawTextW(hdc, L"功能操作 (鼠标悬停可看每个按钮的说明)", -1, &r,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            /* section label: log */
            SelectObject(hdc, g_hFontSection);
            SetTextColor(hdc, UI_TEXT_DIM);
            r.left = S(UI_PAD); r.top = S(LOGLBL_Y);
            r.right = S(400); r.bottom = S(LOGLBL_Y + 20);
            DrawTextW(hdc, L"运行日志", -1, &r,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            /* hint right of the "minimize" checkbox */
            SelectObject(hdc, g_hFontHint);
            SetTextColor(hdc, UI_TEXT_DIM);
            r.left = S(UI_PAD + 312); r.top = S(CHK_Y);
            r.right = S(BTN2_W) * 2 + S(UI_PAD); r.bottom = S(CHK_Y + 24);
            DrawTextW(hdc, L"取消勾选 = 收起到托盘 (不占任务栏)", -1, &r,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER);

            /* rounded border around log area */
            if (g_hwndLog) {
                RECT rcLog;
                GetWindowRect(g_hwndLog, &rcLog);
                MapWindowPoints(NULL, hwnd, (LPPOINT)&rcLog, 2);
                InflateRect(&rcLog, S(3), S(3));
                FrameRoundedRect(hdc, rcLog, S(6), UI_BORDER);
            }

            /* 右下角状态区:
               鼠标停在按钮上 -> 显示该按钮的一句话功能说明 (紫色点)
               否则            -> 显示最近一次操作状态 (按结果着色) */
            dot = UI_ACCENT;
            if (g_hoverHint[0]) {
                dot = RGB(136,84,208);          /* 说明文字用紫色, 与状态区分 */
            } else if (strstr(g_statusText, "completed") || strstr(g_statusText, "success")) {
                dot = UI_SUCCESS;
            } else if (strstr(g_statusText, "fail") ||
                       strstr(g_statusText, "ERROR") ||
                       strstr(g_statusText, "Cannot")) {
                dot = UI_DANGER;
            }

            cy = g_rcStatusArea.top + S(17);
            b = CreateSolidBrush(dot);
            ob = (HBRUSH)SelectObject(hdc, b);
            op = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
            Ellipse(hdc, g_rcStatusArea.left, cy - S(5),
                    g_rcStatusArea.left + S(10), cy + S(5));
            SelectObject(hdc, ob);
            SelectObject(hdc, op);
            DeleteObject(b);

            r = g_rcStatusArea;
            r.left += S(18);
            SelectObject(hdc, g_hFontHint);
            if (g_hoverHint[0]) {
                wcsncpy(wstat, g_hoverHint, 255);
                wstat[255] = L'\0';
                SetTextColor(hdc, UI_TEXT);
            } else {
                wstat[0] = L'\0';
                /* 状态串是 UpdateStatus 用 CP_ACP 存的 (见那里的 MultiByteToWideChar),
                   这里必须用同一个代码页, 否则以后加中文状态就会显示成乱码 */
                MultiByteToWideChar(CP_ACP, 0, g_statusText, -1, wstat, 256);
                wstat[255] = L'\0';
                SetTextColor(hdc, UI_TEXT_DIM);
            }
            DrawTextW(hdc, wstat, -1, &r,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

            EndPaint(hwnd, &ps);
            return 0;
        }

        /* background handled in WM_PAINT (no flicker) */
        case WM_ERASEBKGND:
            return 1;

        /* owner-drawn buttons */
        case WM_DRAWITEM: {
            const DRAWITEMSTRUCT* dis = (const DRAWITEMSTRUCT*)lParam;
            if (dis->CtlType == ODT_BUTTON) {
                DrawCustomButton(dis);
                return TRUE;
            }
            break;
        }

        /* dark theme for edit / static controls */
        case WM_CTLCOLOREDIT: {
            HDC hdc = (HDC)wParam;
            HWND hCtl = (HWND)lParam;
            if (hCtl == g_hwndLog) {
                SetTextColor(hdc, RGB(170, 218, 190));
                SetBkColor(hdc, UI_LOG_BG);
                return (LRESULT)g_hbrLogBg;
            }
            SetTextColor(hdc, UI_TEXT);
            SetBkColor(hdc, UI_INPUT_BG);
            return (LRESULT)g_hbrInputBg;
        }

        case WM_CTLCOLORBTN: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, UI_TEXT);
            SetBkColor(hdc, UI_BG);
            return (LRESULT)g_hbrBg;
        }

        case WM_CTLCOLORSTATIC: {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, UI_TEXT_DIM);
            SetBkColor(hdc, UI_BG);
            return (LRESULT)g_hbrBg;
        }

        /* remove standard frame (custom title bar) */
        case WM_NCCALCSIZE:
            if (wParam) {
                if (IsZoomed(hwnd)) {
                    RECT* r = (RECT*)lParam;
                    r->left += 8; r->top += 8;
                    r->right -= 8; r->bottom -= 8;
                }
                return 0;
            }
            break;

        /* resize borders + title drag hit-testing */
        case WM_NCHITTEST: {
            POINT pt;
            RECT rcw;
            int m = S(8);
            BOOL L, R, T, B;

            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            GetWindowRect(hwnd, &rcw);

            L = pt.x < rcw.left + m;
            R = pt.x > rcw.right - m;
            T = pt.y < rcw.top + m;
            B = pt.y > rcw.bottom - m;

            if (T && L) return HTTOPLEFT;
            if (T && R) return HTTOPRIGHT;
            if (B && L) return HTBOTTOMLEFT;
            if (B && R) return HTBOTTOMRIGHT;
            if (L) return HTLEFT;
            if (R) return HTRIGHT;
            if (T) return HTTOP;
            if (B) return HTBOTTOM;

            ScreenToClient(hwnd, &pt);
            if (pt.y < g_titleH) {
                if (PtInRect(&g_rcTitleClose, pt)) return HTCLIENT;
                if (PtInRect(&g_rcTitleMin, pt)) return HTCLIENT;
                return HTCAPTION;
            }
            break;
        }

        /* avoid classic frame flash on activate changes */
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wParam, -1);

        case WM_MOUSEMOVE: {
            POINT pt;
            int hv = 0;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            if (pt.y < g_titleH) {
                if (PtInRect(&g_rcTitleClose, pt)) hv = 2;
                else if (PtInRect(&g_rcTitleMin, pt)) hv = 1;
            }
            if (hv != g_hoverTitle) {
                g_hoverTitle = hv;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (!g_titleTracking) {
                TRACKMOUSEEVENT tme;
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                tme.dwHoverTime = 0;
                if (TrackMouseEvent(&tme)) g_titleTracking = TRUE;
            }
            break;
        }

        case WM_MOUSELEAVE:
            g_titleTracking = FALSE;
            if (g_hoverTitle) {
                g_hoverTitle = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            break;

        /* custom title bar buttons */
        case WM_LBUTTONUP: {
            POINT pt;
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
            if (PtInRect(&g_rcTitleClose, pt)) {
                PostMessage(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
                return 0;
            }
            if (PtInRect(&g_rcTitleMin, pt)) {
                PostMessage(hwnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);
                return 0;
            }
            break;
        }

        /* 全局热键: 鼠标被 WH_MOUSE_LL 冻结时的唯一可靠入口 */
        case WM_HOTKEY:
            switch (wParam) {
            case ID_HOTKEY_ESCAPE:
                AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+X -> emergency escape\r\n");
                OnKillProcesses(hwnd);
                UpdateStatus(hwnd, "Status: emergency escape done");
                return 0;
            case ID_HOTKEY_PANEL:
                /* 显示/隐藏主面板: 面板收起时也能用, 所以必须是全局热键 */
                if (IsWindowVisible(hwnd)) {
                    HideToTray(hwnd);
                } else {
                    ShowMainWindow(hwnd);
                    AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+H -> panel shown\r\n");
                }
                return 0;
            case ID_HOTKEY_STICKY:
                AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+T -> sticky small window toggle\r\n");
                OnSmallStickyToggle(hwnd);
                return 0;
            case ID_HOTKEY_DESKTOP: {
                char from[128] = "";
                if (RestoreDefaultDesktop(from, sizeof(from))) {
                    AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+D -> desktop restored (was '%s')\r\n",
                              from[0] ? from : "?");
                    UpdateStatus(hwnd, "Status: desktop restored");
                } else {
                    AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+D -> already on Default\r\n");
                }
                return 0;
            }
            case ID_HOTKEY_CLICK:
                EmergencyClick();
                AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+E -> emergency click sent "
                                "(dwExtraInfo=0x12452)\r\n");
                return 0;
            /* 备用入口: 面板被关掉/点不到时也能切换 (主入口是主界面的按钮) */
            case ID_HOTKEY_SCREEN:
                AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+M -> screen-monitor guard toggle\r\n");
                OnScreenGuardToggle(hwnd);
                return 0;
            case ID_HOTKEY_QUIT:
                AppendLog(hwnd, "\r\n[Hotkey] Ctrl+Alt+Q -> exit\r\n");
                DestroyWindow(hwnd);
                return 0;
            default:
                break;
            }
            break;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case ID_BTN_UNLOCK_KB:
                    OnUnlockKeyboard(hwnd);
                    break;
                case ID_BTN_KILL_PROC:
                    OnKillProcesses(hwnd);
                    break;
                case ID_BTN_UNINSTALL:
                    OnUninstallDriver(hwnd);
                    break;
                case ID_BTN_SMALLSTICKY:
                    OnSmallStickyToggle(hwnd);
                    break;
                case ID_BTN_SCREENGUARD:
                    OnScreenGuardToggle(hwnd);
                    break;
                case ID_CHK_TRAY: {
                    BOOL tb = (IsDlgButtonChecked(hwnd, ID_CHK_TRAY) == BST_CHECKED);
                    WriteMinimizeMode(tb);
                    AppendLog(hwnd, "[UI] broadcast minimize -> %s\r\n",
                              tb ? "taskbar (real minimize)"
                                 : "tray (hidden; toggle sticky off to restore)");
                    break;
                }
                case ID_CHK_SHUT:
                    OnShutGuardToggle(hwnd);
                    break;
                case ID_BTN_CHECK_STATUS:
                    OnCheckStatus(hwnd);
                    break;
                case ID_BTN_EXIT:
                    /* 必须 DestroyWindow: 直接 PostQuitMessage 会跳过 WM_DESTROY,
                       托盘图标就会留在通知区直到资源管理器刷新。 */
                    DestroyWindow(hwnd);
                    break;
            }
            /* refresh toggle button labels + status area */
            {
                HWND hS = GetDlgItem(hwnd, ID_BTN_SMALLSTICKY);
                if (hS) InvalidateRect(hS, NULL, TRUE);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;

        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                AppendLog(hwnd, "[UI] WM_SIZE(SIZE_MINIMIZED) -> hide to tray\r\n");
                FileLog("[UI] minimize -> hide to tray");
                HideToTray(hwnd);
                return 0;
            }
            LayoutControls(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            return 0;

        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = S(712);
            mmi->ptMinTrackSize.y = S(616);
            return 0;
        }

        /* Per-Monitor V2: re-layout on DPI change */
        case WM_DPICHANGED: {
            UINT dpiNew = LOWORD(wParam);
            if (dpiNew >= 96 && dpiNew != g_dpi) {
                RECT* prc = (RECT*)lParam;
                g_dpi = dpiNew;
                if (prc) {
                    SetWindowPos(hwnd, NULL, prc->left, prc->top,
                                 prc->right - prc->left,
                                 prc->bottom - prc->top,
                                 SWP_NOZORDER | SWP_NOACTIVATE);
                }
                DestroyUIFonts();
                CreateUIFonts();
                ApplyControlFonts(hwnd);   /* 控件不能继续拿着已删除的 HFONT */
                LayoutControls(hwnd);
                InvalidateRect(hwnd, NULL, TRUE);
            }
            return 0;
        }

        /* 事件驱动小窗化: 广播窗口出现后 150ms 复核一次 (兜住 Show 之后再 Resize) */
        case WM_TIMER:
            if (wParam == IDT_DEMOTE_RECHECK) {
                KillTimer(hwnd, IDT_DEMOTE_RECHECK);
                FlushPendingDemote();
                return 0;
            }
            if (wParam == IDT_TRAY_RETRY) {
                KillTimer(hwnd, IDT_TRAY_RETRY);
                TrayAdd(hwnd);
                return 0;
            }
            if (wParam == IDT_BUILD_CHECK) {
                KillTimer(hwnd, IDT_BUILD_CHECK);
                BuildStatusReport(hwnd, FALSE);   /* 只在旧 DLL 时警告 */
                return 0;
            }
            break;

        case WM_DESTROY:
            /* 停止所有后台线程 */
            {
                int i;
                for (i = ID_HOTKEY_ESCAPE; i <= ID_HOTKEY_LAST; i++)
                    UnregisterHotKey(hwnd, i);
            }
            TrayRemove(hwnd);
            InterlockedExchange(&g_deskGuardStop, 1);
            StopSmallWin(hwnd);
            DestroyUIFonts();
            PostQuitMessage(0);
            return 0;

        /* 关闭 = 收进托盘 (真正退出走托盘菜单) */
        case WM_CLOSE:
            AppendLog(hwnd, "[UI] WM_CLOSE -> hide to tray\r\n");
            HideToTray(hwnd);
            return 0;

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* 让本窗口不出现在任何抓屏里 —— 老师端监控/录屏/截图都拍不到工具界面。
   WDA_EXCLUDEFROMCAPTURE(0x11, Win10 2004+): 直接从抓屏画面里排除;
   更老的系统退化成 WDA_MONITOR(1): 抓屏里是一块黑, 至少不泄露界面内容。
   注意这是"窗口级"设置, 对 BitBlt/PrintWindow/DXGI 抓屏都生效。 */
static void ExcludeOwnWindowFromCapture(HWND hwnd)
{
    DWORD aff = 0;
    BOOL  ok;

    if (!hwnd) return;
    ok = SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
    if (!ok) ok = SetWindowDisplayAffinity(hwnd, WDA_MONITOR);
    if (!GetWindowDisplayAffinity(hwnd, &aff)) aff = 0;
    FileLog("[Screen] own window capture affinity=0x%lx (%s)",
            (unsigned long)aff,
            aff == WDA_EXCLUDEFROMCAPTURE ? "excluded from captures"
                                          : (aff == WDA_MONITOR ? "black in captures"
                                                                : "NOT excluded"));
}

/* ========== 主函数 ========== */

/* 解析命令行参数 */
static void ParseCommandLine(LPSTR lpCmdLine) {
    if (!lpCmdLine) return;

    char cmd[1024];
    strncpy(cmd, lpCmdLine, sizeof(cmd) - 1);
    cmd[sizeof(cmd) - 1] = '\0';

    /* 转小写便于比较 */
    for (char* p = cmd; *p; p++) *p = (char)tolower((unsigned char)*p);

    if (strstr(cmd, "/silent") || strstr(cmd, "-silent")) {
        g_silentMode = TRUE;
    }
    if (strstr(cmd, "/auto") || strstr(cmd, "-auto")) {
        g_autoMode = TRUE;
    }
}

/* 静默模式执行 (无GUI) */
static int RunSilentMode(void) {
    /* 分配控制台用于输出 */
    BOOL allocated = FALSE;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        allocated = AllocConsole();
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || allocated) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    /* 同时写入日志文件 (exe同级目录) */
    char logPath[MAX_PATH];
    GetModuleFileName(NULL, logPath, MAX_PATH);
    PathRemoveFileSpec(logPath);
    PathAppend(logPath, "bypass_silent.log");
    g_logFile = fopen(logPath, "w");

    printf("========================================\n");
    printf("  Classroom Bypass Tool v1.1 (Silent)\n");
    printf("========================================\n\n");

    printf("[Silent] Running full auto bypass...\n");
    AutoAllCore(NULL);
    printf("\n[Silent] Done. Keyboard unlocked and locks removed.\n");
    printf("[Silent] See bypass_silent.log for details.\n");

    if (g_logFile) {
        fclose(g_logFile);
        g_logFile = NULL;
    }

    /* 静默模式下短暂等待, 让用户看到输出 */
    Sleep(2000);
    return 0;
}

/* Log is overwritten on every run: truncate the previous bypass_hook.log here.
   The file itself is written by the injected DLL with FILE_APPEND_DATA (3~4
   processes append concurrently), so use CREATE_ALWAYS rather than DeleteFile:
   processes left over from the previous run still hold the handle, deleting the
   file would make them write into an orphaned inode and this run's log would
   lose those lines. */
static void TruncateHookLog(void) {
    char   dir[MAX_PATH];
    char   logPath[MAX_PATH];
    char*  p;
    HANDLE h;

    if (!GetModuleFileNameA(NULL, dir, sizeof(dir))) return;
    p = strrchr(dir, '\\');
    if (!p) return;
    *(p + 1) = '\0';
    _snprintf(logPath, sizeof(logPath) - 1, "%sbypass_hook.log", dir);
    logPath[sizeof(logPath) - 1] = '\0';

    h = CreateFileA(logPath, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    LogInit();                       /* 日志临界区必须最先就绪 (静默模式也会用) */
    ParseCommandLine(lpCmdLine);

    /* ★ 早期单实例检查：必须在提权之前做，否则第二次双击 bypass.exe 会把
       LPE / UAC 整条链路跑完，最后才发现工具已经在运行。
       这里只是"探测"（查完就关句柄）：真正持有互斥体的是后面提权成功的那
       个实例。提权链路自己拉起的实例（命令行带 --elevate=uac）跳过探测。
       --silent 保持老行为（允许多个静默实例）。 */
    if (!g_silentMode && !Elevate_IsSpawnedChildCmd(lpCmdLine)) {
        HANDLE hMutex = CreateMutexW(NULL, FALSE, L"ClassroomBypassTool_SingleInstance");
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND hOld = FindWindowW(WINDOW_CLASS, NULL);
            CloseHandle(hMutex);
            if (hOld) {
                ShowWindow(hOld, SW_SHOW);
                ShowWindow(hOld, SW_RESTORE);
                /* 已提权窗口可能拒收普通权限进程的 SetForegroundWindow，
                   所以优先走放行过的消息让老实例自己到前台。 */
                if (!PostMessageW(hOld, WM_BYPASS_SHOW, 0, 0))
                    SetForegroundWindow(hOld);
            }
            return 0;
        }
        if (hMutex) CloseHandle(hMutex);
    }

    /* Elevation (src\elevate.c): mode=uac/lpe/manual/off from
       bin\bypass_elevate.ini (--elevate= overrides).  "lpe" tries the
       WalletService payload in bin\lpe\ first and falls back to the UAC
       prompt and finally to the manual "run as administrator" instructions. */
    if (!IsUserAnAdmin()) {
        Elevate_Boot(NULL, lpCmdLine, g_silentMode);
        return 0;
    }
    Elevate_NoteElevated();

    /* 记录 UI 线程 (供 AppendLog 判断是否需要 PostMessage), 并打开所需特权 */
    g_uiThreadId = GetCurrentThreadId();
    EnableRequiredPrivileges();

    /* 单实例: 托盘程序重复启动会出现多个图标, 这里直接把已有窗口叫出来 */
    if (!g_silentMode) {
        HANDLE hMutex = CreateMutexW(NULL, FALSE, L"ClassroomBypassTool_SingleInstance");
        if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND hOld = FindWindowW(WINDOW_CLASS, NULL);
            if (hOld) {
                ShowWindow(hOld, SW_SHOW);
                ShowWindow(hOld, SW_RESTORE);
                SetForegroundWindow(hOld);
            }
            CloseHandle(hMutex);
            return 0;
        }
        /* 故意不关闭句柄: 进程存活期间它要一直有效 */
    }

    /* 配置 + 共享状态（必须在注入 DLL 之前） */
    StateInit();
    ScreenGuardLoad();          /* 屏幕监控守卫: 读 bin\bypass_screen.ini */
    ShutGuardLoad();            /* 远程重启/关机守卫: 读 bin\bypass_shut.ini */

    /* this run only: truncate before any DLL starts appending.
       An instance spawned by the elevation chain must NOT truncate - the
       launcher's elevation history and the handshake line live in the same
       file until the launcher has read them. */
    if (!Elevate_IsSpawnedChildCmd(lpCmdLine))
        TruncateHookLog();

    /* 静默模式: 直接执行, 不创建窗口 */
    if (g_silentMode) {
        return RunSilentMode();
    }
    
    /* 初始化通用控件 */
    InitDpiApis();

    /* Per-Monitor V2 DPI awareness (dynamic, no manifest) */
    {
        HMODULE hU = GetModuleHandleW(L"user32.dll");
        typedef BOOL (WINAPI *PFN_SetProcessDpiAwarenessContext)(HANDLE);
        PFN_SetProcessDpiAwarenessContext pSetCtx = hU ?
            (PFN_SetProcessDpiAwarenessContext)(void*)
            GetProcAddress(hU, "SetProcessDpiAwarenessContext") : NULL;

        if (pSetCtx) {
            if (!pSetCtx((HANDLE)DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
                pSetCtx((HANDLE)DPI_AWARENESS_CONTEXT_SYSTEM_AWARE);
        } else {
            HMODULE hS = LoadLibraryW(L"shcore.dll");
            if (hS) {
                typedef HRESULT (WINAPI *PFN_SPD)(int);
                PFN_SPD pSPD = (PFN_SPD)(void*)
                    GetProcAddress(hS, "SetProcessDpiAwareness");
                if (pSPD) pSPD(2); /* PER_MONITOR_AWARE */
                FreeLibrary(hS);
            }
        }
    }

    g_dpi = GetWindowDpiSafe(NULL);

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    /* 注册窗口类 */
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.cbWndExtra = 0;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIcon(NULL, IDI_SHIELD);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszMenuName = NULL;
    wc.lpszClassName = WINDOW_CLASS;
    wc.hIconSm = LoadIcon(NULL, IDI_SHIELD);

    if (!RegisterClassExW(&wc)) {
        MessageBoxW(NULL, L"窗口注册失败!", L"错误", MB_ICONERROR);
        return -1;
    }
    
    /* 创建窗口 (无边框 + 自绘标题栏)
       WS_EX_APPWINDOW: 窗口可见时任务栏上有按钮 (托盘图标被 Windows 折叠进
       "^" 里也找得回来); 最小化/关闭时 HideToTray 会 SW_HIDE, 按钮随之消失,
       真正干活的还是托盘图标. */
    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        WINDOW_CLASS,
        WINDOW_TITLE,
        WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU |
        WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, S(712), S(748),
        NULL, NULL, hInstance, NULL);

    if (!hwnd) {
        MessageBoxW(NULL, L"窗口创建失败!", L"错误", MB_ICONERROR);
        return -1;
    }

    /* 放行低完整性进程发来的 WM_BYPASS_SHOW：第二次双击 bypass.exe 的新实例
       (通常是普通权限) 要能唤醒已提权的本窗口（UIPI 默认拒收）。 */
    ChangeWindowMessageFilterEx(hwnd, WM_BYPASS_SHOW, MSGFLT_ALLOW, NULL);

    /* 本窗口永远不进抓屏: 老师端监控/录屏时看不到工具界面 (托盘里点的
       「屏蔽屏幕监控」只负责"定格画面", 这一条负责"别把工具拍进去")。 */
    ExcludeOwnWindowFromCapture(hwnd);

    /* Windows 11: rounded window corners (optional, dynamic load) */
    {
        typedef HRESULT (WINAPI *PFN_DwmSetWindowAttribute)(
            HWND, DWORD, LPCVOID, DWORD);
        HMODULE hDwm = LoadLibraryA("dwmapi.dll");
        if (hDwm) {
            PFN_DwmSetWindowAttribute pfn =
                (PFN_DwmSetWindowAttribute)(void*)GetProcAddress(
                    hDwm, "DwmSetWindowAttribute");
            if (pfn) {
                DWORD pref = 2; /* DWMWCP_ROUND */
                pfn(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */,
                    &pref, sizeof(pref));
            }
            FreeLibrary(hDwm);
        }
    }

    /* ★ 启动时强制正常显示。
       以前用 nCmdShow: 如果启动方式带 SW_SHOWMINIMIZED, WM_SIZE(SIZE_MINIMIZED)
       会立刻把窗口收进托盘 —— 表现就是"运行程序后 GUI 自动隐藏"。
       托盘程序在启动时应当让用户看到界面, 之后最小化/关闭才收进托盘。 */
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    AppendLog(hwnd, "[UI] window shown at startup (nCmdShow=%d)\r\n", nCmdShow);

    /* 等注入器把 DLL 送进去之后, 检查"进程里的 DLL 是不是磁盘上的这个":
       旧 DLL 不会因为重启本程序而替换 (注入器会跳过已注入的进程)。 */
    SetTimer(hwnd, IDT_BUILD_CHECK, 4000, NULL);

    /* 如果指定了 /auto, 自动执行一次一键绕过。
       ★ 投递到消息循环里跑: AutoAllCore 里有 Sleep + 等驱动停止 (最坏几十秒),
         在 ShowWindow 之后同步跑, 窗口立刻"未响应"。 */
    if (g_autoMode) {
        PostMessage(hwnd, WM_APP_AUTOALL, 0, 0);
    }
    
    /* 消息循环 */
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    return (int)msg.wParam;
}
