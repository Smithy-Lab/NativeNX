// SPDX-License-Identifier: GPL-3.0-or-later
// The POSIX calls of src/compat_win32.h that need <windows.h>.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <errno.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/types.h>

// A read at an offset that leaves the file's position alone, as pread does (several threads read one file):
// ReadFile with the offset in an OVERLAPPED
ssize_t pread(int fd, void *buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    o.Offset = (DWORD)((uint64_t)off & 0xFFFFFFFFu), o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD got = 0;
    if (h == INVALID_HANDLE_VALUE || n > 0xFFFFFFFFu) { errno = EINVAL; return -1; }
    if (!ReadFile(h, buf, (DWORD)n, &got, &o)) {
        if (GetLastError() == ERROR_HANDLE_EOF) return 0;
        errno = EIO;
        return -1;
    }
    return (ssize_t)got;
}

// ---- sys/mman.h ----
#include <sys/mman.h>
static DWORD page_prot(int prot) {
    if (prot & PROT_EXEC) return prot & PROT_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
    return prot & PROT_WRITE ? PAGE_READWRITE : prot & PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
}
// whether [a, a + len) lies inside one reservation already
static int reserved_already(void *a, size_t len) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(a, &mi, sizeof mi) || mi.State == MEM_FREE) return 0;
    MEMORY_BASIC_INFORMATION last;
    if (!VirtualQuery((char *)a + len - 1, &last, sizeof last)) return 0;
    return last.State != MEM_FREE && last.AllocationBase == mi.AllocationBase;
}
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off) {
    (void)off;
    if (fd >= 0 || !(flags & MAP_ANONYMOUS) || !len) { errno = !len ? EINVAL : ENOTSUP; return MAP_FAILED; }
    DWORD pp = page_prot(prot);
    if (addr && (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))) {
        if (reserved_already(addr, len)) {
            if (flags & MAP_FIXED_NOREPLACE) { errno = EEXIST; return MAP_FAILED; }
            VirtualFree(addr, len, MEM_DECOMMIT);   // (fresh pages: what was there is gone)
            if (prot == PROT_NONE) return addr;
            return VirtualAlloc(addr, len, MEM_COMMIT, pp) ? addr : (errno = ENOMEM, MAP_FAILED);
        }
        void *p = VirtualAlloc(addr, len, MEM_RESERVE | (prot != PROT_NONE ? MEM_COMMIT : 0), pp);
        if (p == addr) return p;
        if (p) VirtualFree(p, 0, MEM_RELEASE);
        errno = (flags & MAP_FIXED_NOREPLACE) ? EEXIST : ENOMEM;
        return MAP_FAILED;
    }
    void *p = addr ? VirtualAlloc(addr, len, MEM_RESERVE | (prot != PROT_NONE ? MEM_COMMIT : 0), pp) : NULL;   // a hint
    if (!p) p = VirtualAlloc(NULL, len, MEM_RESERVE | (prot != PROT_NONE ? MEM_COMMIT : 0), pp);
    if (!p) { errno = ENOMEM; return MAP_FAILED; }
    return p;
}
int munmap(void *addr, size_t len) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(addr, &mi, sizeof mi)) { errno = EINVAL; return -1; }
    if (mi.AllocationBase == addr) {   // the whole mapping when it is all of it
        MEMORY_BASIC_INFORMATION r;
        size_t total = 0;
        for (char *p = addr; VirtualQuery(p, &r, sizeof r) && r.AllocationBase == addr; p += r.RegionSize) total += r.RegionSize;
        if (total <= ((len + 0xFFF) & ~(size_t)0xFFF)) return VirtualFree(addr, 0, MEM_RELEASE) ? 0 : (errno = EINVAL, -1);
    }
    return VirtualFree(addr, len, MEM_DECOMMIT) ? 0 : (errno = EINVAL, -1);
}
int mprotect(void *addr, size_t len, int prot) {
    DWORD pp = page_prot(prot), old;
    char *p = addr, *end = (char *)addr + len;
    while (p < end) {   // region by region: committed ones changed, reserved ones committed when given access
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(p, &mi, sizeof mi) || mi.State == MEM_FREE) { errno = ENOMEM; return -1; }
        char *re = (char *)mi.BaseAddress + mi.RegionSize;
        size_t n = (size_t)((re < end ? re : end) - p);
        if (mi.State == MEM_COMMIT) { if (!VirtualProtect(p, n, pp, &old)) { errno = EACCES; return -1; } }
        else if (prot != PROT_NONE && !VirtualAlloc(p, n, MEM_COMMIT, pp)) { errno = ENOMEM; return -1; }
        p += n;
    }
    return 0;
}
int madvise(void *addr, size_t len, int advice) {
    if (advice == MADV_DONTNEED) {   // the pages' contents may go: given back and committed again (zero)
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(addr, &mi, sizeof mi) && mi.State == MEM_COMMIT) {
            DWORD prot = mi.Protect;
            VirtualFree(addr, len, MEM_DECOMMIT);
            VirtualAlloc(addr, len, MEM_COMMIT, prot);
        }
    }
    return 0;
}

// ---- dlfcn.h ----
#include <dlfcn.h>
static __thread char g_dlerr[300];
static __thread int g_dlerr_set;
static void dl_fail(const char *what) {
    snprintf(g_dlerr, sizeof g_dlerr, "%s: Windows error %lu", what, (unsigned long)GetLastError());
    g_dlerr_set = 1;
}
void *dlopen(const char *file, int mode) {
    (void)mode;
    if (!file) return GetModuleHandleW(NULL);
    HMODULE h = LoadLibraryExA(file, NULL, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
    if (!h) h = LoadLibraryA(file);
    if (!h) dl_fail(file);
    return h;
}
void *dlsym(void *handle, const char *name) {
    void *p = (void *)(uintptr_t)GetProcAddress((HMODULE)handle, name);
    if (!p) dl_fail(name);
    return p;
}
int dlclose(void *handle) { return FreeLibrary((HMODULE)handle) ? 0 : -1; }
char *dlerror(void) {
    if (!g_dlerr_set) return NULL;
    g_dlerr_set = 0;
    return g_dlerr;
}

// ---- the rest of compat_win32.h ----
#include <stdarg.h>
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o = {0};
    o.Offset = (DWORD)((uint64_t)off & 0xFFFFFFFFu), o.OffsetHigh = (DWORD)((uint64_t)off >> 32);
    DWORD put = 0;
    if (h == INVALID_HANDLE_VALUE || n > 0xFFFFFFFFu) { errno = EINVAL; return -1; }
    if (!WriteFile(h, buf, (DWORD)n, &put, &o)) { errno = EIO; return -1; }
    return (ssize_t)put;
}
static int utf16(const char *s, wchar_t *w, int n) { return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0 ? 0 : -1; }
int nxl_rename(const char *from, const char *to) {
    wchar_t a[32768], b[32768];
    if (utf16(from, a, 32768) || utf16(to, b, 32768)) { errno = ENAMETOOLONG; return -1; }
    if (MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
    DWORD e = GetLastError();
    errno = e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? ENOENT : e == ERROR_ACCESS_DENIED ? EACCES : e == ERROR_ALREADY_EXISTS ? EEXIST : EIO;
    return -1;
}
int fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (cmd == F_GETPATH) {   // the file's full path, '/' between folders, without Windows' \\?\ prefix
        HANDLE h = (HANDLE)_get_osfhandle(fd);
        wchar_t w[32768];
        DWORD k = h == INVALID_HANDLE_VALUE ? 0 : GetFinalPathNameByHandleW(h, w, 32768, FILE_NAME_NORMALIZED);
        if (!k || k >= 32768) { errno = EBADF; return -1; }
        const wchar_t *s = !wcsncmp(w, L"\\\\?\\UNC\\", 8) ? w + 6 : !wcsncmp(w, L"\\\\?\\", 4) ? w + 4 : w;
        char *out = arg;
        if (WideCharToMultiByte(CP_UTF8, 0, s, -1, out, 1024, NULL, NULL) <= 0) { errno = ENAMETOOLONG; return -1; }
        if (s != w + 4 && s != w) out[0] = '/', out[1] = '/';   // (a UNC path: \\server\share)
        for (char *c = out; *c; c++) if (*c == '\\') *c = '/';
        return 0;
    }
    if (cmd == F_GETFD || cmd == F_GETFL) return 0;
    return 0;   // F_SETFD, F_SETFL: nothing to change
}
int nxl_read_memory(const void *from, void *to, size_t n, size_t *got) {
    SIZE_T k = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), from, to, n, &k)) return -1;
    *got = k;
    return 0;
}

// ---- dirent.h ----
#include <dirent.h>
#include <sys/stat.h>
struct nxl_dir {
    HANDLE f;
    WIN32_FIND_DATAW fd;
    int first, slot;   // first: fd holds an entry not returned yet
    struct dirent e;
    char path[4096];
    wchar_t pattern[4200];
};
// the folders open now, for dirfd and fstatat (a number from 1 << 28 up)
static SRWLOCK g_dirs_lock = SRWLOCK_INIT;
static DIR *g_dirs[256];
enum { DIRFD_BASE = 1 << 28 };
DIR *opendir(const char *path) {
    DIR *d = calloc(1, sizeof *d);
    if (!d) { errno = ENOMEM; return NULL; }
    snprintf(d->path, sizeof d->path, "%s", path);
    char pat[4200];
    snprintf(pat, sizeof pat, "%s/*", path);
    if (MultiByteToWideChar(CP_UTF8, 0, pat, -1, d->pattern, 4200) <= 0) { free(d); errno = ENAMETOOLONG; return NULL; }
    d->f = FindFirstFileW(d->pattern, &d->fd);
    if (d->f == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND) {   // (a folder with nothing in it, not even . and ..: a drive's root)
            free(d);
            errno = e == ERROR_PATH_NOT_FOUND || e == ERROR_INVALID_NAME ? ENOENT : e == ERROR_DIRECTORY ? ENOTDIR : e == ERROR_ACCESS_DENIED ? EACCES : EIO;
            return NULL;
        }
    } else d->first = 1;
    d->slot = -1;
    return d;
}
struct dirent *readdir(DIR *d) {
    if (d->f == INVALID_HANDLE_VALUE) return NULL;
    if (!d->first && !FindNextFileW(d->f, &d->fd)) return NULL;
    d->first = 0;
    if (WideCharToMultiByte(CP_UTF8, 0, d->fd.cFileName, -1, d->e.d_name, sizeof d->e.d_name, NULL, NULL) <= 0) d->e.d_name[0] = '?', d->e.d_name[1] = 0;
    d->e.d_type = d->fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT && d->fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK ? DT_LNK
                : d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? DT_DIR : DT_REG;
    d->e.d_reclen = sizeof d->e;
    d->e.d_ino = 0;
    return &d->e;
}
void rewinddir(DIR *d) {
    if (d->f != INVALID_HANDLE_VALUE) FindClose(d->f);
    d->f = FindFirstFileW(d->pattern, &d->fd);
    d->first = d->f != INVALID_HANDLE_VALUE;
}
int closedir(DIR *d) {
    if (!d) return -1;
    if (d->slot >= 0) { AcquireSRWLockExclusive(&g_dirs_lock); g_dirs[d->slot] = NULL; ReleaseSRWLockExclusive(&g_dirs_lock); }
    if (d->f != INVALID_HANDLE_VALUE) FindClose(d->f);
    free(d);
    return 0;
}
int dirfd(DIR *d) {
    if (d->slot >= 0) return DIRFD_BASE + d->slot;
    AcquireSRWLockExclusive(&g_dirs_lock);
    for (int i = 0; i < 256; i++) if (!g_dirs[i]) { g_dirs[i] = d, d->slot = i; break; }
    ReleaseSRWLockExclusive(&g_dirs_lock);
    if (d->slot < 0) { errno = EMFILE; return -1; }
    return DIRFD_BASE + d->slot;
}
int fstatat(int fd, const char *name, struct stat *st, int flags) {
    (void)flags;
    if (fd == AT_FDCWD || name[0] == '/' || (name[0] && name[1] == ':')) return stat(name, st);
    char p[4200];
    AcquireSRWLockShared(&g_dirs_lock);
    DIR *d = fd >= DIRFD_BASE && fd < DIRFD_BASE + 256 ? g_dirs[fd - DIRFD_BASE] : NULL;
    if (d) snprintf(p, sizeof p, "%s/%s", d->path, name);
    ReleaseSRWLockShared(&g_dirs_lock);
    if (!d) { errno = EBADF; return -1; }
    return stat(p, st);
}

// ---- sys/resource.h ----
#include <sys/resource.h>
int getrlimit(int resource, struct rlimit *r) {
    (void)resource;
    r->rlim_cur = r->rlim_max = 8192;   // (the UCRT's descriptors go to 8192)
    return 0;
}
static void tv_of(FILETIME f, struct timeval *tv) {
    uint64_t us = (((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 10;
    tv->tv_sec = (long)(us / 1000000), tv->tv_usec = (long)(us % 1000000);
}
int getrusage(int who, struct rusage *u) {
    FILETIME c, e, k, us;
    memset(u, 0, sizeof *u);
    BOOL ok = who == RUSAGE_THREAD ? GetThreadTimes(GetCurrentThread(), &c, &e, &k, &us) : GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &us);
    if (!ok) { errno = EINVAL; return -1; }
    tv_of(us, &u->ru_utime), tv_of(k, &u->ru_stime);
    return 0;
}
long sysconf(int name) {
    if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) return (long)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (name == _SC_PAGESIZE) return 4096;
    errno = EINVAL;
    return -1;
}

// ---- sys/random.h ----
#include <bcrypt.h>
#include <sys/random.h>
ssize_t getrandom(void *buf, size_t n, unsigned flags) {
    (void)flags;
    if (n > 0xFFFFFFFFu) n = 0xFFFFFFFFu;
    return BCryptGenRandom(NULL, buf, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) ? (errno = EIO, -1) : (ssize_t)n;
}

// ---- clock_nanosleep (compat_win32.h) ----
#include <pthread.h>
#include <time.h>
#undef clock_nanosleep
int nxl_clock_nanosleep(int clock, int flags, const struct timespec *t, struct timespec *rem) {
    struct timespec d = *t;
    if (flags & TIMER_ABSTIME) {   // the time left until then on that clock
        struct timespec now;
        clock_gettime(clock, &now);
        d.tv_sec -= now.tv_sec, d.tv_nsec -= now.tv_nsec;
        if (d.tv_nsec < 0) d.tv_sec--, d.tv_nsec += 1000000000;
        if (d.tv_sec < 0) return 0;
    }
    return nanosleep(&d, rem) ? errno : 0;
}
