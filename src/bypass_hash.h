/* FNV-1a 32 over a file or a buffer.
 *
 * Why: the UI used to decide "is the DLL in this process current?" from the
 * PE TimeDateStamp.  That is a *link time*, not an identity: two rebuilds in
 * the same second, reproducible builds (SOURCE_DATE_EPOCH) and hand-edited
 * binaries all keep the same stamp.  With hot reload the Loader maps a cache
 * copy, so "which bytes did this process actually load" is a content question.
 *
 * Header-only (static functions) so the 32-bit Loader and the 64-bit UI share
 * one implementation without another object file.
 */
#ifndef BYPASS_HASH_H
#define BYPASS_HASH_H

#define HR_HASH_INIT  2166136261u
#define HR_HASH_PRIME 16777619u

static unsigned long HrFnv1aBuf(unsigned long h, const void* buf, size_t n)
{
    const unsigned char* p = (const unsigned char*)buf;
    size_t i;

    for (i = 0; i < n; i++) {
        h ^= (unsigned long)p[i];
        h *= HR_HASH_PRIME;
    }
    return h;
}

/* 1 = hashed, 0 = cannot open / empty file.  `out` and `sizeOut` may be NULL. */
static int HrFnv1aFile(const char* path, unsigned long* out, unsigned long* sizeOut)
{
    HANDLE        f;
    unsigned char buf[16384];
    DWORD         rd = 0;
    unsigned long h = HR_HASH_INIT;
    unsigned long total = 0;

    if (out)     *out = 0;
    if (sizeOut) *sizeOut = 0;
    if (!path || !path[0]) return 0;

    /* share read+write: the build may be rewriting it while we look */
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;

    while (ReadFile(f, buf, sizeof(buf), &rd, NULL) && rd > 0) {
        h = HrFnv1aBuf(h, buf, rd);
        total += rd;
    }
    CloseHandle(f);

    if (total == 0) return 0;
    if (out)     *out = h;
    if (sizeOut) *sizeOut = total;
    return 1;
}

#endif /* BYPASS_HASH_H */
