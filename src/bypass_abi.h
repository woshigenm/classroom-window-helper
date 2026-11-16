/*
 * bypass_abi.h -- the stable interface between the hook Loader and the Core.
 *
 *   bypass_hook32.dll   = Loader (owns the hook entries, trampolines, IAT
 *                         patching, logging, paths).  It is the module that
 *                         gets injected; it is NOT rebuilt when only the
 *                         interception logic changes.
 *   bypass_hook_core.dll = Core (all Hook_* implementations, window logic,
 *                         screen/shutdown guards).  Loaded by the Loader.
 *
 * Rules:
 *   - HookTable / HrCoreApi / HrHost carry abi_version + size.  Any field
 *     change requires HR_ABI_VERSION++.  The Loader refuses a Core whose
 *     Core_Init returns 0 (Core must check host->abi_version FIRST and do
 *     nothing else when it does not match, otherwise FreeLibrary would UAF).
 *   - Once Core_Init returns 1 the Loader MUST NOT FreeLibrary it: the Core
 *     may already have threads/hooks.  A bad table is simply not published.
 *   - The Loader installs every inline hook and IAT patch and passes the
 *     trampolines as HrHost.real.*.  The Core must call the originals through
 *     those pointers, never through GetProcAddress on a hooked entry.
 *   - Adding a new interception point requires a Loader change (dispatcher +
 *     install + HrReals/HookTable field) => restart the classroom client.
 *     Changing a Hook_* body only requires a new Core.
 *   - 32-bit only (see loader.c).
 *
 * Pure C, same compiler for both modules.  ASCII only.
 */

#ifndef BYPASS_ABI_H
#define BYPASS_ABI_H

#include <windows.h>

#define HR_ABI_VERSION   3       /* v3: in_hook_count + uef_push/pop */
#define HR_CORE_NAME     "bypass_hook_core.dll"
#define HR_LOADER_NAME   "bypass_hook32.dll"
#define HR_CORE_INIT     "Core_Init"
#define HR_CACHE_DIR     "core_cache"
#define HR_CTL_PIPE      "\\\\.\\pipe\\bypass_core_ctl_"

/* ---------------- function signatures (shared by both sides) ---------------- */

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
typedef HWND     (WINAPI *PFN_CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD,
                                              int, int, int, int, HWND, HMENU,
                                              HINSTANCE, LPVOID);
typedef BOOL     (WINAPI *PFN_SetForegroundWindow)(HWND);
typedef BOOL     (WINAPI *PFN_BringWindowToTop)(HWND);
typedef HWND     (WINAPI *PFN_SetActiveWindow)(HWND);
typedef BOOL     (WINAPI *PFN_ExitWindowsEx)(UINT, DWORD);

typedef BOOL     (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES,
                                              LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                              LPVOID, LPCSTR, LPSTARTUPINFOA,
                                              LPPROCESS_INFORMATION);
typedef BOOL     (WINAPI *PFN_CreateProcessW)(LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                              LPSECURITY_ATTRIBUTES, BOOL, DWORD,
                                              LPVOID, LPCWSTR, LPSTARTUPINFOW,
                                              LPPROCESS_INFORMATION);
typedef UINT     (WINAPI *PFN_WinExec)(LPCSTR, UINT);

typedef BOOL     (WINAPI *PFN_BitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
typedef HDC      (WINAPI *PFN_CreateDCW)(LPCWSTR, LPCWSTR, LPCWSTR, const DEVMODEW*);
typedef HDC      (WINAPI *PFN_CreateDCA)(LPCSTR, LPCSTR, LPCSTR, const DEVMODE*);
typedef HDC      (WINAPI *PFN_GetDC)(HWND);
typedef HDC      (WINAPI *PFN_GetWindowDC)(HWND);

typedef HINSTANCE (WINAPI *PFN_ShellExecuteA)(HWND, LPCSTR, LPCSTR, LPCSTR, LPCSTR, INT);
typedef HINSTANCE (WINAPI *PFN_ShellExecuteW)(HWND, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, INT);

typedef BOOL (WINAPI *PFN_InitiateSystemShutdownA)(LPSTR, LPSTR, DWORD, BOOL, BOOL);
typedef BOOL (WINAPI *PFN_InitiateSystemShutdownW)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL);
typedef BOOL (WINAPI *PFN_InitiateSystemShutdownExA)(LPSTR, LPSTR, DWORD, BOOL, BOOL, DWORD);
typedef BOOL (WINAPI *PFN_InitiateSystemShutdownExW)(LPWSTR, LPWSTR, DWORD, BOOL, BOOL, DWORD);

/* ---------------- host -> core ---------------- */

typedef void (*HR_LOG_FN)(const char* fmt, ...);

/* Patching is owned by the Loader; returns the number of IAT slots rewritten
   in this pass (the Core only uses it for its "attached" log line). */
typedef long (*HR_PATCH_FN)(void);

typedef struct HrReals {
    void* SetWindowPos;
    void* SetWindowLongW;
    void* SetWindowLongA;
    void* SetWindowLongPtrW32;   /* SetWindowLongPtrW is a macro on x86 */
    void* SetWindowLongPtrA32;
    void* MoveWindow;
    void* SetWindowPlacement;
    void* ShowWindow;
    void* ShowWindowAsync;
    void* ShowCursor;
    void* SetCursor;
    void* SwitchDesktop;
    void* SetWindowsHookExA;
    void* SetWindowsHookExW;
    void* CreateWindowExW;
    void* SetForegroundWindow;
    void* BringWindowToTop;
    void* SetActiveWindow;
    void* ExitWindowsEx;

    void* CreateProcessA;
    void* CreateProcessW;
    void* WinExec;

    void* BitBlt;
    void* CreateDCW;
    void* CreateDCA;
    void* GetDC;
    void* GetWindowDC;

    void* ShellExecuteA;
    void* ShellExecuteW;

    void* InitiateSystemShutdownA;
    void* InitiateSystemShutdownW;
    void* InitiateSystemShutdownExA;
    void* InitiateSystemShutdownExW;
} HrReals;

/* P2: the Core stores reloadable state here.  The Loader keeps the store and
   snapshots it before before_unload(); on a failed reload it restores the
   snapshot BEFORE loading the previous Core, so that Core_Init reads the
   pre-unload state (not a half-written blob from the new Core). */
typedef int (*HR_BLOB_PUT)(const char* key, const void* data, unsigned int size);
typedef int (*HR_BLOB_GET)(const char* key, void* out, unsigned int* size);

/* P2b: the Loader owns the hook refcount and the UnhandledExceptionFilter
   chain, so a Core can be unloaded without leaving pointers into it. */
typedef LPTOP_LEVEL_EXCEPTION_FILTER (*HR_UEF_PUSH_FN)(LPTOP_LEVEL_EXCEPTION_FILTER);
typedef LPTOP_LEVEL_EXCEPTION_FILTER (*HR_UEF_POP_FN)(void);

typedef struct HrHost {
    unsigned int abi_version;
    unsigned int size;
    const char*  base_dir;      /* "...\\bin\\" (with trailing backslash)   */
    const char*  loader_path;   /* full path of bypass_hook32.dll            */
    HR_LOG_FN    log;
    HR_PATCH_FN  patch_modules;
    HR_BLOB_PUT  blob_put;
    HR_BLOB_GET  blob_get;
    volatile LONG* in_hook_count;   /* Loader's count of all dispatcher calls */
    HR_UEF_PUSH_FN uef_push;
    HR_UEF_POP_FN  uef_pop;
    HrReals      real;
} HrHost;

/* ---------------- core -> loader ---------------- */

typedef struct HrHooks {
    PFN_SetWindowPos        SetWindowPos;
    PFN_SetWindowLongW      SetWindowLongW;
    PFN_SetWindowLongA      SetWindowLongA;
    PFN_SetWindowLongPtrW   SetWindowLongPtrW32;
    PFN_SetWindowLongPtrA   SetWindowLongPtrA32;
    PFN_MoveWindow          MoveWindow;
    PFN_SetWindowPlacement  SetWindowPlacement;
    PFN_ShowWindow          ShowWindow;
    PFN_ShowWindowAsync     ShowWindowAsync;
    PFN_ShowCursor          ShowCursor;
    PFN_SetCursor           SetCursor;
    PFN_SwitchDesktop       SwitchDesktop;
    PFN_SetWindowsHookExA   SetWindowsHookExA;
    PFN_SetWindowsHookExW   SetWindowsHookExW;
    PFN_CreateWindowExW     CreateWindowExW;
    PFN_SetForegroundWindow SetForegroundWindow;
    PFN_BringWindowToTop    BringWindowToTop;
    PFN_SetActiveWindow     SetActiveWindow;
    PFN_ExitWindowsEx       ExitWindowsEx;

    PFN_CreateProcessA      CreateProcessA;
    PFN_CreateProcessW      CreateProcessW;
    PFN_WinExec             WinExec;

    PFN_BitBlt              BitBlt;
    PFN_CreateDCW           CreateDCW;
    PFN_CreateDCA           CreateDCA;
    PFN_GetDC               GetDC;
    PFN_GetWindowDC         GetWindowDC;

    PFN_ShellExecuteA       ShellExecuteA;
    PFN_ShellExecuteW       ShellExecuteW;

    PFN_InitiateSystemShutdownA   InitiateSystemShutdownA;
    PFN_InitiateSystemShutdownW   InitiateSystemShutdownW;
    PFN_InitiateSystemShutdownExA InitiateSystemShutdownExA;
    PFN_InitiateSystemShutdownExW InitiateSystemShutdownExW;
} HrHooks;

typedef struct HrCoreApi {
    unsigned int abi_version;
    unsigned int size;
    HrHooks      hooks;
    const char*  build_id;      /* __DATE__ " " __TIME__                     */
    int        (*on_frame)(void);       /* optional, P2 tick (may be NULL)   */
    int        (*before_unload)(void);  /* P2 drain; 0 = refuse unload       */
} HrCoreApi;

typedef int (__cdecl *PFN_Core_Init)(const HrHost* host, HrCoreApi* api);

#endif /* BYPASS_ABI_H */
