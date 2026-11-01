// SPDX-License-Identifier: GPL-3.0-or-later
// Force-included into nxnative's sources: the Darwin functions and constants they use, on Linux (and on Windows, over
// src/compat_win32.h's POSIX calls and compat/nxnative/win32/include's headers).
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/param.h>
#include <time.h>
#include <unistd.h>

// clock_gettime_nsec_np: a clock in nanoseconds
static inline uint64_t clock_gettime_nsec_np(clockid_t clk) {
    struct timespec ts;
    clock_gettime(clk, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#ifndef CLOCK_UPTIME_RAW
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC
#endif
#ifndef CLOCK_MONOTONIC_RAW_APPROX
#define CLOCK_MONOTONIC_RAW_APPROX CLOCK_MONOTONIC
#endif

// fcntl(F_GETPATH): the path of an open descriptor (Linux: its /proc/self/fd link; Windows: src/compat_win32.c)
#if !defined(F_GETPATH) && !defined(_WIN32)
#define F_GETPATH 0x7F000050
static inline int nxl_fcntl(int fd, int cmd, ...) {
    va_list ap;
    va_start(ap, cmd);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (cmd != F_GETPATH) return fcntl(fd, cmd, arg);
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, (char *)arg, MAXPATHLEN - 1);
    if (n < 0) return -1;
    ((char *)arg)[n] = 0;
    return 0;
}
#define fcntl(...) nxl_fcntl(__VA_ARGS__)
#endif

// mmap(NULL, ...): on macOS the kernel's choice lands below 2^39, inside what a Horizon program can address, and
// nxnative hands such memory straight to the game (the shared fonts, NVN's virtual pool reservations). Linux places it
// near 2^47, where the game's code can misread it, so a mapping with no address of its own goes into a guest-addressable
// window instead (nxl_host_mmap, compat.c); fixed and hinted mappings are unchanged
#include <sys/mman.h>
void *nxl_host_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
#define mmap(a, l, p, f, d, o) nxl_host_mmap(a, l, p, f, d, o)

// Darwin's relative timed wait (compat_dm.c)
#include <pthread.h>
#include <time.h>
int nxl_cond_wait_rel(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel);

// what differs on Windows (MinGW): struct tm has no zone name or offset, timegm is _mkgmtime, an aligned allocation is
// freed by its own call
#ifdef _WIN32
#define NXL_LIST_SEP ";"   // (between the folders of DM_ROMFS_OVERLAY; src/platform.h has the same)
#include <malloc.h>
const char *nxl_tm_zone(const struct tm *tm);
long nxl_tm_gmtoff(const struct tm *tm);
#define NXL_TM_ZONE(tm) nxl_tm_zone(&(tm))
#define NXL_TM_GMTOFF(tm) nxl_tm_gmtoff(&(tm))
#define nxl_timegm(tm) _mkgmtime(tm)
static inline int nxl_aligned_alloc(void **p, size_t align, size_t n) { *p = _aligned_malloc(n, align); return *p ? 0 : ENOMEM; }
#define nxl_aligned_free _aligned_free
#else
#define NXL_LIST_SEP ":"
#define NXL_TM_ZONE(tm) ((tm).tm_zone)
#define NXL_TM_GMTOFF(tm) ((tm).tm_gmtoff)
#define nxl_timegm(tm) timegm(tm)
#define nxl_aligned_alloc posix_memalign
#define nxl_aligned_free free
#endif
