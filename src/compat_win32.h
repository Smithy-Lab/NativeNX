// SPDX-License-Identifier: GPL-3.0-or-later
// The smaller POSIX calls nxlinux and NativeNX use, on Windows (MinGW-w64 with the UCRT). The build includes this first
// in every C file there (cmake/windows.cmake, -include), without <windows.h> (its names, FIXED, ERROR, min ..., would
// clash with the programs'): what needs it is in src/compat_win32.c; what is bigger (processes, folders, the program's own path) is in
// src/platform.h. Text is UTF-8 throughout: the programs' manifest (packaging/windows/) sets UTF-8 as their code page,
// so fopen, opendir and the -A calls take UTF-8 names.
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <direct.h>
#include <fcntl.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>
#include <io.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#ifdef __cplusplus
extern "C" {
#endif

// mkdir takes no mode on Windows
#define mkdir(path, mode) _mkdir(path)
// the UCRT's access() rejects X_OK (it ends the program): a program there only has to exist
#undef X_OK
#define X_OK F_OK

static inline int setenv(const char *name, const char *value, int overwrite) {
    if (!overwrite && getenv(name)) return 0;
    return _putenv_s(name, value) ? -1 : 0;
}
static inline int unsetenv(const char *name) { return _putenv_s(name, "") ? -1 : 0; }   // (an empty value removes it)
static inline char *realpath(const char *path, char *resolved) {
    char *r = _fullpath(resolved, path, resolved ? PATH_MAX : 0);
    if (r) for (char *c = r; *c; c++) if (*c == '\\') *c = '/';
    return r;
}
static inline char *strcasestr(const char *h, const char *n) {
    size_t l = strlen(n);
    for (; *h; h++) if (!_strnicmp(h, n, l)) return (char *)(uintptr_t)h;
    return l ? NULL : (char *)(uintptr_t)h;
}
static inline void *memmem(const void *h, size_t hn, const void *n, size_t nn) {
    if (!nn) return (void *)(uintptr_t)h;
    for (const char *p = (const char *)h; hn >= nn; p++, hn--) if (*p == *(const char *)n && !memcmp(p, n, nn)) return (void *)(uintptr_t)p;
    return NULL;
}
static inline struct tm *localtime_r(const time_t *t, struct tm *out) { return localtime_s(out, t) ? NULL : out; }
static inline struct tm *gmtime_r(const time_t *t, struct tm *out) { return gmtime_s(out, t) ? NULL : out; }
// a read and a write at an offset (src/compat_win32.c)
ssize_t pread(int fd, void *buf, size_t n, off_t off);
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off);
// clock_nanosleep until a time on the monotonic clock too (MinGW's pthreads take only the real-time clock for that)
struct timespec;
int nxl_clock_nanosleep(int clock, int flags, const struct timespec *t, struct timespec *rem);
#define clock_nanosleep(c, f, t, r) nxl_clock_nanosleep(c, f, t, r)
// the system's numbers: processors, the page size
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_ONLN 84
#define _SC_NPROCESSORS_CONF 83
long sysconf(int name);
static inline int getpagesize(void) { return 4096; }
#define timegm _mkgmtime
static inline int fsync(int fd) { return _commit(fd); }
static inline ssize_t readlink(const char *path, char *buf, size_t n) { (void)path, (void)buf, (void)n; errno = EINVAL; return -1; }   // (no links)
// rename replaces what is at the new name, as on POSIX (the C runtime's refuses to)
int nxl_rename(const char *from, const char *to);
#define rename(from, to) nxl_rename(from, to)
// fcntl: the descriptor flags (nothing to do: descriptors are not inherited, reads wait), and Darwin's F_GETPATH (the
// path of an open file, which nxnative asks for)
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define F_GETPATH 50
#ifndef O_NONBLOCK
#define O_NONBLOCK 0x800
#endif
int fcntl(int fd, int cmd, ...);
// descriptors are not inherited by the programs started (O_CLOEXEC's work)
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; }
    return l;
}
static inline size_t strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    return dl == n ? n + strlen(s) : dl + strlcpy(d + dl, s, n - dl);
}
// links need the user's permission there: the callers copy the file instead
static inline int symlink(const char *target, const char *path) { (void)target, (void)path; errno = ENOSYS; return -1; }
static inline int lstat(const char *path, struct stat *st) { return stat(path, st); }
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
#ifndef S_ISSOCK
#define S_ISSOCK(m) 0
#endif
#ifndef S_ISBLK
#define S_ISBLK(m) 0
#endif
#ifdef __cplusplus
}
#endif
#endif
