/* ============================================================
 *  fake_core.c -- deliberately broken / instrumented Core DLLs for the
 *  Loader tests (build with -DFAKE_MODE=<n>).
 *
 *  0: Core_Init returns 0 (did nothing)   -> Loader may FreeLibrary
 *  1: returns 1, abi_version=999          -> must NOT FreeLibrary
 *  2: returns 1, right ABI, hooks NULL    -> must NOT FreeLibrary
 *  3: returns 1, complete dummy table, before_unload=1, and LoadLibrary(self)
 *     once -> after reload FreeLibrary leaves it mapped (still-mapped test)
 *  4: returns 1, complete dummy table, before_unload=1 (reload success test)
 *
 *  1/2 start a heartbeat thread (%TEMP%\bypass_fake_core_beat.txt); if the
 *  Loader wrongly unloaded them the thread would crash the host.
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

extern IMAGE_DOS_HEADER __ImageBase;

#if FAKE_MODE >= 3
static LONG_PTR WINAPI DummyHook(void)
{
    return 0;
}

static void FillDummyHooks(HrHooks* h)
{
    void** p = (void**)h;
    size_t i, n = sizeof(*h) / sizeof(void*);

    for (i = 0; i < n; i++) p[i] = (void*)DummyHook;
}

static int __cdecl FakeBeforeUnload(void)
{
    return 1;
}
#endif  /* FAKE_MODE >= 3 */

#if FAKE_MODE == 1 || FAKE_MODE == 2
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
#endif  /* FAKE_MODE == 1 || FAKE_MODE == 2 */

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
    CreateThread(NULL, 0, BeatThread, NULL, 0, NULL);
# elif FAKE_MODE == 2
    api->abi_version = HR_ABI_VERSION;
    api->hooks.CreateProcessW = (PFN_CreateProcessW)(void*)0x1;  /* incomplete */
    CreateThread(NULL, 0, BeatThread, NULL, 0, NULL);
# else
    api->abi_version = HR_ABI_VERSION;
    FillDummyHooks(&api->hooks);
    api->before_unload = FakeBeforeUnload;
#  if FAKE_MODE == 3
    {   /* extra reference: FreeLibrary will not unmap this module */
        char own[MAX_PATH];
        if (GetModuleFileNameA((HMODULE)&__ImageBase, own, sizeof(own)))
            LoadLibraryA(own);
    }
#  endif
# endif
    return 1;
#endif
}
