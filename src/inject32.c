/*
 * inject32.exe —— 把 32 位 hook DLL 注入到课堂渲染进程
 *
 * 为什么必须是 32 位程序:
 *   MultiRender.exe / ScreenRender.exe / MultiClient.exe 全部是 PE32 (x86)。
 *   64 位进程不能把 32 位 DLL 塞进 32 位进程里 (CreateRemoteThread 的
 *   入口地址宽度不匹配), 所以注入器本身必须编成 i686。
 *
 * 用法:
 *   inject32.exe <hook.dll 的完整路径> [日志文件路径]
 *
 * 说明:
 *   - 自己按进程名枚举目标, 不需要外面传 PID。
 *   - 已经注入过的进程会跳过 (查模块列表), 不会让 DLL 的引用计数一直涨。
 *   - 每次调用都会把结果追加写到日志文件, 方便 bypass.exe 显示。
 *
 * 退出码:
 *   0 = 至少有一个进程注入成功, 或者目标进程里已经全部有 hook
 *   1 = 一个都没成功
 *   2 = 参数错误
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* 只注入学生端进程。教师端 (Teacher/ScreenSender/...) 不碰:
   教师机上的 ScreenRender 是"广播预览窗", 学生端才是真正的广播窗。 */
static const char* TARGETS[] = {
    "ScreenRender.exe",
    "MultiRender.exe",
    "MultiClient.exe",
    "AudioRender.exe",
    "AudioSender.exe",
    "Student.exe",
    "MmcStudent.exe",          /* 本机学生端界面进程 (没有 Student.exe 的版本) */
    "SharedDesktop.exe",
    "StudentScreenHelper.exe",
    "hooktest32.exe",          /* 自测载体: 只在本机测试时存在 */
    NULL
};

static HANDLE g_log = INVALID_HANDLE_VALUE;
static int   g_ok;      /* 成功(注入或已存在)的进程数 */
static int   g_try;     /* 命中的目标进程数 */
static int   g_quiet;   /* 静默: 只记录"真的动手了"的行 */

static void LogClose(void)
{
    if (g_log != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

static void Log(const char* fmt, ...)
{
    char    buf[512];
    char    fmtA[512];
    wchar_t wfmt[512];
    va_list ap;

    /* 源码里的格式串是 UTF-8, %s 填的路径是 ACP —— 先统一转成 ACP,
       否则写进日志文件的中文会一半正常一半乱码 (bypass.exe 也按 ACP 读)。 */
    if (MultiByteToWideChar(CP_UTF8, 0, fmt, -1, wfmt, 512) > 0 &&
        WideCharToMultiByte(CP_ACP, 0, wfmt, -1, fmtA, sizeof(fmtA), NULL, NULL) > 0)
        fmt = fmtA;

    va_start(ap, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';

    printf("%s\n", buf);
    fflush(stdout);

    /* 多个 inject32 实例 (Student/MultiClient/ScreenRender 同时 self-inject)
       会写同一个日志文件。fopen(...,"a") 是"seek 到末尾再写", 两个进程会
       算出同一个末尾互相覆盖 -> 日志半行被吃掉、换行丢失。
       FILE_APPEND_DATA 由文件系统保证每次都追加到末尾。 */
    if (g_log != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        char  line[768];
        int   n;
        DWORD wrote = 0;

        GetLocalTime(&st);
        n = _snprintf(line, sizeof(line) - 1, "%02d:%02d:%02d  %s\r\n",
                      st.wHour, st.wMinute, st.wSecond, buf);
        if (n < 0) n = (int)strlen(line);
        line[sizeof(line) - 1] = '\0';
        if (n > (int)sizeof(line) - 1) n = (int)sizeof(line) - 1;
        WriteFile(g_log, line, (DWORD)n, &wrote, NULL);
    }
}

/* 把 DLL 复制到本机 %TEMP% 再注入。
   共享文件夹 (VMware HGFS / 网络盘) 不支持 PE 的 image section 映射,
   目标进程里 LoadLibraryW 会直接返回 0, 而且原因在对方进程里拿不到。
   这里先在自己进程里做一次 LOAD_LIBRARY_AS_IMAGE_RESOURCE 自检,
   失败就把 DLL (连同配置文件和安全阀) 落到本地临时目录, 再从那里注入。 */
/* 本地固定盘（C:\ 等）不需要复制到 %TEMP%：只有共享目录/网络盘/可移动盘
   才可能不支持 PE image section 映射。HGFS 即使显示成固定盘也要排除。 */
static BOOL IsLocalFixedPath(const char* path)
{
    char root[4];
    char fs[32] = "";
    UINT type;

    if (!path || !path[0]) return FALSE;
    if (path[0] == '\\' && path[1] == '\\') return FALSE;   /* UNC */
    if (path[1] != ':' || path[2] != '\\') return FALSE;

    root[0] = path[0];
    root[1] = ':';
    root[2] = '\\';
    root[3] = '\0';
    type = GetDriveTypeA(root);
    if (type != DRIVE_FIXED) return FALSE;

    if (GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, sizeof(fs))) {
        if (!_stricmp(fs, "HGFS") || !_stricmp(fs, "vmhgfs") ||
            !_stricmp(fs, "VBOXSF") || !_stricmp(fs, "UNCREF"))
            return FALSE;
    }
    return TRUE;
}

static BOOL StageToLocal(const char* src, char* out, size_t outSize,
                         wchar_t* outW, size_t outWCount)
{
    char  tmp[MAX_PATH];
    char  dir[MAX_PATH];
    char  dst[MAX_PATH];
    char* base;
    static const char* SWITCH[] = {
        "bypass_config.ini", "bypass_vd.ini", "bypass_hook.off", NULL
    };
    int   i;

    if (!GetTempPathA(sizeof(tmp), tmp)) return FALSE;

    _snprintf(dir, sizeof(dir) - 1, "%sbypass_stage\\", tmp);
    dir[sizeof(dir) - 1] = '\0';
    CreateDirectoryA(dir, NULL);        /* 已存在也无所谓 */

    base = strrchr((char*)src, '\\');
    base = base ? base + 1 : (char*)src;
    _snprintf(dst, sizeof(dst) - 1, "%s%s", dir, base);
    dst[sizeof(dst) - 1] = '\0';

    /* DLL 落地。★ 已经有进程把它加载着（上次注入的残留）时 CopyFileA 会因
       共享冲突失败 —— 但那个本地副本恰恰是能用的（映射过的 DLL 文件本身可以
       再 LoadLibrary）。这时直接沿用旧副本，比整个注入失败强得多；
       只有 dst 也不存在才是真的失败。 */
    if (!CopyFileA(src, dst, FALSE)) {
        DWORD err = GetLastError();
        if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) {
            Log("[dll] 复制到本地失败 err=%lu, 但 %s 已存在 -> 沿用旧副本 "
                "(可能是上一轮注入的进程还加载着它)",
                (unsigned long)err, dst);
        } else {
            Log("[dll] 复制到本地失败: %s err=%lu", dst, (unsigned long)err);
            return FALSE;
        }
    }

    /* 配置/安全阀跟着走: 源目录有就复制, 没有就删掉本地旧的那份,
       否则本地残留的 bypass_hook.off 会让钩子一直是关的。
       ★ bypass_vd.ini 也要跟: DLL 按自己的位置找它, 少了它转存模式下
         虚拟桌面固定会静默失效。 */
    for (i = 0; SWITCH[i]; i++) {
        char s[MAX_PATH], d[MAX_PATH];
        _snprintf(s, sizeof(s) - 1, "%.*s%s", (int)(base - src), src, SWITCH[i]);
        s[sizeof(s) - 1] = '\0';
        _snprintf(d, sizeof(d) - 1, "%s%s", dir, SWITCH[i]);
        d[sizeof(d) - 1] = '\0';
        if (GetFileAttributesA(s) != INVALID_FILE_ATTRIBUTES)
            CopyFileA(s, d, FALSE);
        else
            DeleteFileA(d);
    }

    if (!MultiByteToWideChar(CP_ACP, 0, dst, -1, outW, (int)outWCount))
        return FALSE;

    _snprintf(out, outSize - 1, "%s", dst);
    out[outSize - 1] = '\0';
    return TRUE;
}

/* DLL 是否已经在目标进程里加载过。
   ★ 当问的是 loader (bypass_hook32.dll) 时, 旧单 DLL (bypass_hook_single.dll)
     也算"已经加载": 两者同时打 inline hook 会互相覆盖/装不上。 */
static BOOL IsModuleLoaded(DWORD pid, const char* dllBaseName)
{
    HANDLE          snap;
    MODULEENTRY32   me;
    BOOL            found = FALSE;
    BOOL            alsoSingle;

    alsoSingle = (dllBaseName &&
                  !_stricmp(dllBaseName, "bypass_hook32.dll")) ? TRUE : FALSE;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return FALSE;

    me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (!_stricmp(me.szModule, dllBaseName) ||
                (alsoSingle && !_stricmp(me.szModule, "bypass_hook_single.dll"))) {
                found = TRUE;
                break;
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

/* 目标是不是 64 位进程。32 位注入器注不进去, 直接跳过 —— 否则会打一堆
   误导性的 "LoadLibraryW 返回 0"。
   ⚠ 必须先在"本进程"上判断 WOW64: 如果注入器自己跑在 32 位 Windows 上,
     IsWow64Process 对所有进程都返回 FALSE, 会把 32 位目标误判成 64 位。 */
static BOOL Is64BitProcess(DWORD pid)
{
    HANDLE hp;
    BOOL   selfWow64 = FALSE, targetWow64 = FALSE;

    if (!IsWow64Process(GetCurrentProcess(), &selfWow64) || !selfWow64)
        return FALSE;                    /* 32 位系统: 不存在 64 位进程 */

    hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return FALSE;
    if (!IsWow64Process(hp, &targetWow64)) { CloseHandle(hp); return FALSE; }
    CloseHandle(hp);
    return !targetWow64;
}

static BOOL InjectOne(DWORD pid, const wchar_t* dllPathWide)
{
    const wchar_t* wpath = dllPathWide;
    size_t         bytes = (wcslen(wpath) + 1) * sizeof(wchar_t);
    HANDLE         hp;
    void*          mem;
    HANDLE         th;
    FARPROC        loadLib;
    DWORD          written = 0;
    DWORD          code = 0;
    BOOL           ok = FALSE;

    hp = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
                     PROCESS_VM_WRITE | PROCESS_VM_READ |
                     PROCESS_QUERY_INFORMATION,
                     FALSE, pid);
    if (!hp) {
        Log("  [失败] OpenProcess pid=%lu err=%lu (多半是权限, 需要以管理员运行)",
            (unsigned long)pid, (unsigned long)GetLastError());
        return FALSE;
    }

    mem = VirtualAllocEx(hp, NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) {
        Log("  [失败] VirtualAllocEx pid=%lu err=%lu",
            (unsigned long)pid, (unsigned long)GetLastError());
        CloseHandle(hp);
        return FALSE;
    }

    if (!WriteProcessMemory(hp, mem, wpath, bytes, &written) || written != bytes) {
        Log("  [失败] WriteProcessMemory pid=%lu err=%lu",
            (unsigned long)pid, (unsigned long)GetLastError());
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        CloseHandle(hp);
        return FALSE;
    }

    loadLib = GetProcAddress(GetModuleHandleA("kernel32.dll"), "LoadLibraryW");
    if (!loadLib) {
        Log("  [失败] 找不到 LoadLibraryW");
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        CloseHandle(hp);
        return FALSE;
    }

    th = CreateRemoteThread(hp, NULL, 0, (LPTHREAD_START_ROUTINE)loadLib,
                            mem, 0, NULL);
    if (!th) {
        Log("  [失败] CreateRemoteThread pid=%lu err=%lu",
            (unsigned long)pid, (unsigned long)GetLastError());
        VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
        CloseHandle(hp);
        return FALSE;
    }

    if (WaitForSingleObject(th, 5000) != WAIT_OBJECT_0) {
        /* ★ 超时不要 VirtualFreeEx: 远程线程可能还在用这块路径内存,
           释放后它读到的是已解除映射的地址 -> 目标进程直接崩。宁可泄漏几十字节。 */
        Log("  [失败] 等待远程线程超时 pid=%lu (路径内存保留, 不释放)",
            (unsigned long)pid);
        CloseHandle(th);
        CloseHandle(hp);
        return FALSE;
    }

    GetExitCodeThread(th, &code);
    if (code == 0) {
        Log("  [失败] 目标进程里 LoadLibraryW 返回 0 pid=%lu "
            "(DLL 可能依赖缺失或位数不对)", (unsigned long)pid);
    } else {
        ok = TRUE;
    }

    CloseHandle(th);
    VirtualFreeEx(hp, mem, 0, MEM_RELEASE);
    CloseHandle(hp);
    return ok;
}

static BOOL IsTargetName(const char* name)
{
    int i;
    for (i = 0; TARGETS[i]; i++) {
        if (!_stricmp(name, TARGETS[i])) return TRUE;
    }
    return FALSE;
}

int main(int argc, char** argv)
{
    char            dllA[MAX_PATH];
    wchar_t         dllW[MAX_PATH * 2];
    char            baseA[MAX_PATH];
    char*           p;
    const char*     dllArg = NULL;
    const char*     logArg = NULL;
    const char*     quietArg = NULL;
    int             pidMode = 0;
    DWORD           onlyPid = 0;
    HANDLE          snap;
    PROCESSENTRY32  pe;

    /* 两种用法:
       inject32.exe <dll> [log] [quiet]                     按进程名枚举
       inject32.exe --pid <pid> <dll> [log] [quiet]         只打一个进程 (调试用) */
    if (argc >= 2 && !_stricmp(argv[1], "--pid")) {
        if (argc < 4) {
            printf("用法: inject32.exe --pid <PID> <bypass_hook32.dll> [日志文件] [quiet]\n");
            return 2;
        }
        pidMode  = 1;
        onlyPid  = (DWORD)atoi(argv[2]);
        dllArg   = argv[3];
        logArg   = (argc >= 5) ? argv[4] : NULL;
        quietArg = (argc >= 6) ? argv[5] : NULL;
    } else {
        if (argc < 2) {
            printf("用法: inject32.exe <bypass_hook32.dll 的完整路径> [日志文件] [quiet]\n");
            return 2;
        }
        dllArg   = argv[1];
        logArg   = (argc >= 3) ? argv[2] : NULL;
        quietArg = (argc >= 4) ? argv[3] : NULL;
    }

    if (logArg) {
        g_log = CreateFileA(logArg, FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    }
    if (quietArg && !_stricmp(quietArg, "quiet")) {
        g_quiet = 1;
    }

    if (!GetFullPathNameA(dllArg, sizeof(dllA), dllA, NULL)) {
        Log("[失败] 无法解析 DLL 路径: %s", dllArg);
        LogClose();
        return 2;
    }
    if (GetFileAttributesA(dllA) == INVALID_FILE_ATTRIBUTES) {
        Log("[失败] 找不到 DLL: %s", dllA);
        LogClose();
        return 2;
    }
    if (!MultiByteToWideChar(CP_ACP, 0, dllA, -1, dllW,
                             (int)(sizeof(dllW) / sizeof(dllW[0])))) {
        Log("[失败] 路径转 Unicode 失败");
        LogClose();
        return 2;
    }

    /* 本地固定盘直接注入，不再复制到 %TEMP%；只有共享/网络/可移动盘
       才需要做 image section 自检和转存。 */
    if (IsLocalFixedPath(dllA)) {
        Log("[dll] %s 在本地固定盘 -> 直接注入（不复制到 %%TEMP%%）", dllA);
    } else {
        char  fs[64] = "?";
        char  root[8];
        DWORD sz = 0;
        HANDLE hs, hf;
        WIN32_FIND_DATAA fd;

        hs = FindFirstFileA(dllA, &fd);
        if (hs != INVALID_HANDLE_VALUE) {
            sz = fd.nFileSizeLow;
            FindClose(hs);
        }
        _snprintf(root, sizeof(root) - 1, "%c:\\", dllA[0]);
        root[sizeof(root) - 1] = '\0';
        GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fs, sizeof(fs));

        Log("[dll] %s  size=%lu  fs=%s", dllA, (unsigned long)sz, fs);

        hf = (HANDLE)LoadLibraryExW(dllW, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        if (hf) {
            Log("[dll] 本机映射自检: OK");
            FreeLibrary(hf);
        } else {
            DWORD mErr = GetLastError();
            char  staged[MAX_PATH];

            Log("[dll] 本机映射自检: FAILED err=%lu (共享/网络目录不支持映像映射时就是这里挂)",
                (unsigned long)mErr);

            if (StageToLocal(dllA, staged, sizeof(staged),
                             dllW, sizeof(dllW) / sizeof(dllW[0]))) {
                Log("[dll] 已转存到本地: %s", staged);
                _snprintf(dllA, sizeof(dllA) - 1, "%s", staged);
                dllA[sizeof(dllA) - 1] = '\0';
                hf = (HANDLE)LoadLibraryExW(dllW, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
                if (hf) {
                    Log("[dll] 转存后自检: OK");
                    FreeLibrary(hf);
                } else {
                    Log("[dll] 转存后自检: 仍然 FAILED err=%lu",
                        (unsigned long)GetLastError());
                }
            }
        }
    }

    /* DLL 的文件名, 用来查是否已注入 */
    p = strrchr(dllA, '\\');
    _snprintf(baseA, sizeof(baseA) - 1, "%s", p ? p + 1 : dllA);
    baseA[sizeof(baseA) - 1] = '\0';

    /* --- 只打一个 PID (自测用) --- */
    if (pidMode) {
        g_try = 1;
        if (Is64BitProcess(onlyPid)) {
            Log("  [跳过] pid=%lu 是 64 位进程, 32 位 hook 注不进去",
                (unsigned long)onlyPid);
        } else if (IsModuleLoaded(onlyPid, baseA)) {
            Log("  [已在] pid=%lu 已经挂着 hook, 跳过", (unsigned long)onlyPid);
            g_ok = 1;
        } else {
            Log("  [注入] pid=%lu", (unsigned long)onlyPid);
            if (InjectOne(onlyPid, dllW)) {
                g_ok = 1;
                Log("  [成功] pid=%lu hook 已生效", (unsigned long)onlyPid);
            }
        }
        if (!g_quiet) Log("[汇总] 1 个目标, 成功 %d 个", g_ok);
        LogClose();
        return (g_ok > 0) ? 0 : 1;
    }

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log("[失败] CreateToolhelp32Snapshot err=%lu", (unsigned long)GetLastError());
        LogClose();
        return 1;
    }

    pe.dwSize = sizeof(pe);
    if (Process32First(snap, &pe)) {
        do {
            if (!IsTargetName(pe.szExeFile)) continue;
            if (Is64BitProcess(pe.th32ProcessID)) continue;   /* 32 位钩子注不进 64 位进程 */

            g_try++;

            if (IsModuleLoaded(pe.th32ProcessID, baseA)) {
                if (!g_quiet) {
                    Log("  [已在] %s (pid=%lu) 已经挂着 hook, 跳过",
                        pe.szExeFile, (unsigned long)pe.th32ProcessID);
                }
                g_ok++;
                continue;
            }

            Log("  [注入] %s (pid=%lu)", pe.szExeFile,
                (unsigned long)pe.th32ProcessID);
            if (InjectOne(pe.th32ProcessID, dllW)) {
                g_ok++;
                Log("  [成功] %s (pid=%lu) hook 已生效",
                    pe.szExeFile, (unsigned long)pe.th32ProcessID);
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);

    if (g_try == 0) {
        if (!g_quiet) {
            Log("[提示] 没找到目标进程 (课堂软件没在运行, 或进程名对不上)");
        }
        LogClose();
        return 1;
    }

    if (!g_quiet) {
        Log("[汇总] 命中 %d 个目标进程, 成功 %d 个", g_try, g_ok);
    }

    LogClose();
    return (g_ok > 0) ? 0 : 1;
}
