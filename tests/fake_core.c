/* ============================================================
 *  fake_core.c -- deliberately broken Core DLLs for the Loader's
 *  failure-path tests (build three times with -DFAKE_MODE=0/1/2).
 *
 *  FAKE_MODE 0: Core_Init returns 0 (did nothing)  -> Loader may FreeLibrary
 *  FAKE_MODE 1: Core_Init returns 1 but abi_version=999
 *                -> Loader must NOT FreeLibrary, table stays NULL
 *  FAKE_MODE 2: Core_Init returns 1 with the right ABI but an incomplete
 *                hook table -> Loader must NOT FreeLibrary, table stays NULL
 *
 *  Modes 1/2 also start a heartbeat thread that keeps appending to
 *  %TEMP%\bypass_fake_core_beat.txt.  If the Loader wrongly FreeLibrary'd a
 *  successfully initialized Core, that thread would execute freed code and
 *  the host process would crash (no VERDICT line).  The script checks both:
 *  no crash AND the heartbeat file exists.
 *
 *  32-bit, C, ASCII only.
 * ============================================================ */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../src/bypass_abi.h"

#ifndef FAKE_MODE
#define FAKE_MODE 0
#endif

#if FAKE_MODE != 0
static DWORD WINAPI BeatThread(LPVOID p)
{
    char path[MAX_PATH];
    DWORD n;

    (void)p;
    n = GetTempPathA(sizeof(path), path);
    if (!n || n >= sizeof(path)) return 0;
    _snprintf(path + n, sizeof(path) - n - 1, "bypass_fake_core_beat.txt");
    path[sizeof(path) - 1] = '\0';

    for (;;) {
        HANDLE f = CreateFileA(path, FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            char  b[64];
            DWORD w = 0;
            int   len = _snprintf(b, sizeof(b) - 1, "beat mode=%d tick=%lu\n",
                                  FAKE_MODE, (unsigned long)GetTickCount());
            if (len > 0) WriteFile(f, b, (DWORD)len, &w, NULL);
            CloseHandle(f);
        }
        Sleep(150);
    }
    return 0;
}
#endif  /* FAKE_MODE != 0 */

__declspec(dllexport) int __cdecl Core_Init(const HrHost* host, HrCoreApi* api)
{
    (void)host;

#if FAKE_MODE == 0
    (void)api;
    return 0;
#else
    memset(api, 0, sizeof(*api));
    api->size = sizeof(*api);
# if FAKE_MODE == 1
    api->abi_version = 999;
# else
    api->abi_version = HR_ABI_VERSION;
    /* only one entry: HooksComplete() must reject the table */
    api->hooks.CreateProcessW = (PFN_CreateProcessW)(void*)0x1;
# endif
    CreateThread(NULL, 0, BeatThread, NULL, 0, NULL);
    return 1;
#endif
}
