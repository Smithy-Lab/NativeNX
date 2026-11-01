// SPDX-License-Identifier: GPL-3.0-or-later
// The Darwin calls nxnative's save handling makes (hle_fs.c), on Windows: extended attributes as NTFS named streams,
// clonefile as a copy, renamex_np's swap as three renames (compat/nxnative/win32/include/sys/*.h).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "win32/include/sys/clonefile.h"
#include "win32/include/sys/xattr.h"

static int wide(const char *s, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0 ? 0 : -1; }
static int set_errno(DWORD e) {
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : e == ERROR_ACCESS_DENIED ? EACCES
          : e == ERROR_ALREADY_EXISTS || e == ERROR_FILE_EXISTS ? EEXIST : e == ERROR_DIR_NOT_EMPTY ? ENOTEMPTY : EIO;
    return -1;
}
// "<path>:nxl.<name>": the stream that holds an attribute
static int stream_name(const char *path, const char *name, wchar_t *w, int n) {
    char s[4200];
    if (snprintf(s, sizeof s, "%s:nxl.%s", path, name) >= (int)sizeof s) { errno = ENAMETOOLONG; return -1; }
    return wide(s, w, n);
}
ssize_t nxl_getxattr(const char *path, const char *name, void *value, size_t size) {
    wchar_t w[4200];
    if (stream_name(path, name, w, 4200)) return -1;
    HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) { errno = ENOATTR; return -1; }   // no such attribute (the file itself may be there)
        return set_errno(e);
    }
    LARGE_INTEGER sz;
    DWORD got = 0;
    ssize_t r = -1;
    if (!GetFileSizeEx(h, &sz)) set_errno(GetLastError());
    else if (!value || !size) r = (ssize_t)sz.QuadPart;   // its size asked for
    else if ((uint64_t)sz.QuadPart > size) errno = ERANGE;
    else if (ReadFile(h, value, (DWORD)sz.QuadPart, &got, NULL)) r = (ssize_t)got;
    else set_errno(GetLastError());
    CloseHandle(h);
    return r;
}
int nxl_setxattr(const char *path, const char *name, const void *value, size_t size) {
    wchar_t w[4200];
    if (stream_name(path, name, w, 4200)) return -1;
    HANDLE h = CreateFileW(w, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return set_errno(GetLastError());
    DWORD put = 0;
    BOOL ok = WriteFile(h, value, (DWORD)size, &put, NULL) && put == size;
    CloseHandle(h);
    return ok ? 0 : set_errno(GetLastError());
}
int nxl_removexattr(const char *path, const char *name) {
    wchar_t w[4200];
    if (stream_name(path, name, w, 4200)) return -1;
    if (DeleteFileW(w)) return 0;
    DWORD e = GetLastError();
    if (e == ERROR_FILE_NOT_FOUND) { errno = ENOATTR; return -1; }
    return set_errno(e);
}

// a folder's own named streams (its attributes) copied onto another folder (a file's go with CopyFileW)
static void copy_dir_streams(const wchar_t *src, const wchar_t *dst) {
    WIN32_FIND_STREAM_DATA fs;
    HANDLE f = FindFirstStreamW(src, FindStreamInfoStandard, &fs, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        // ":nxl.name:$DATA" -> "<src>:nxl.name"
        wchar_t nm[300], a[4400], b[4400];
        wcsncpy(nm, fs.cStreamName, 299), nm[299] = 0;
        wchar_t *t = wcsstr(nm + 1, L":");
        if (t) *t = 0;
        if (!nm[1]) continue;   // (the unnamed stream: a folder has none)
        _snwprintf(a, 4400, L"%ls%ls", src, nm), _snwprintf(b, 4400, L"%ls%ls", dst, nm);
        CopyFileW(a, b, FALSE);
    } while (FindNextStreamW(f, &fs));
    FindClose(f);
}
static int clone_w(const wchar_t *src, const wchar_t *dst) {
    DWORD at = GetFileAttributesW(src);
    if (at == INVALID_FILE_ATTRIBUTES) return set_errno(GetLastError());
    if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) { errno = EEXIST; return -1; }
    if (!(at & FILE_ATTRIBUTE_DIRECTORY)) return CopyFileW(src, dst, TRUE) ? 0 : set_errno(GetLastError());
    if (!CreateDirectoryW(dst, NULL)) return set_errno(GetLastError());
    copy_dir_streams(src, dst);
    wchar_t pat[4400];
    _snwprintf(pat, 4400, L"%ls\\*", src);
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;   // (empty)
    int r = 0;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wchar_t a[4400], b[4400];
        _snwprintf(a, 4400, L"%ls\\%ls", src, fd.cFileName), _snwprintf(b, 4400, L"%ls\\%ls", dst, fd.cFileName);
        r = clone_w(a, b);
    } while (!r && FindNextFileW(f, &fd));
    FindClose(f);
    return r;
}
int clonefile(const char *src, const char *dst, unsigned flags) {
    (void)flags;
    wchar_t a[4200], b[4200];
    if (wide(src, a, 4200) || wide(dst, b, 4200)) { errno = ENAMETOOLONG; return -1; }
    for (wchar_t *c = a; *c; c++) if (*c == L'/') *c = L'\\';
    for (wchar_t *c = b; *c; c++) if (*c == L'/') *c = L'\\';
    return clone_w(a, b);
}
int renamex_np(const char *from, const char *to, unsigned flags) {
    wchar_t a[4200], b[4200], t[4300];
    if (wide(from, a, 4200) || wide(to, b, 4200)) { errno = ENAMETOOLONG; return -1; }
    if (!(flags & RENAME_SWAP)) return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : set_errno(GetLastError());
    _snwprintf(t, 4300, L"%ls.swap-%lu", b, (unsigned long)GetCurrentProcessId());
    if (!MoveFileExW(b, t, 0)) return set_errno(GetLastError());
    if (!MoveFileExW(a, b, 0)) { DWORD e = GetLastError(); MoveFileExW(t, b, 0); return set_errno(e); }
    if (!MoveFileExW(t, a, 0)) return set_errno(GetLastError());
    return 0;
}

// struct tm's zone name and offset from UTC (Windows' struct tm has neither): the C runtime's time zone, with daylight
// saving time when tm is in it
#include <time.h>
const char *nxl_tm_zone(const struct tm *tm) {
    static char names[2][64];
    static int done;
    if (!done) {
        size_t n;
        _tzset();
        for (int i = 0; i < 2; i++) _get_tzname(&n, names[i], sizeof names[i], i);
        done = 1;
    }
    return names[tm->tm_isdst > 0];
}
long nxl_tm_gmtoff(const struct tm *tm) {
    long tz = 0, dst = 0;
    _tzset();
    _get_timezone(&tz);   // seconds west of UTC
    if (tm->tm_isdst > 0) _get_dstbias(&dst);
    return -(tz + dst);
}
