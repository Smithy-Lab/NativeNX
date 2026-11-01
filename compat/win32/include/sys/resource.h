// SPDX-License-Identifier: GPL-3.0-or-later
// getrusage and getrlimit on Windows (src/compat_win32.c): the process's CPU times, and the C runtime's limit on open
// descriptors
#pragma once
#include <sys/time.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef unsigned long long rlim_t;
struct rlimit { rlim_t rlim_cur, rlim_max; };
struct rusage { struct timeval ru_utime, ru_stime; long ru_maxrss; };
#define RLIMIT_NOFILE 7
#define RUSAGE_SELF 0
#define RUSAGE_THREAD 1
int getrlimit(int resource, struct rlimit *r);
int getrusage(int who, struct rusage *u);
static inline int getdtablesize(void) { return 8192; }
#ifdef __cplusplus
}
#endif
