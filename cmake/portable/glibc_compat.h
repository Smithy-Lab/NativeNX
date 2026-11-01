// SPDX-License-Identifier: GPL-3.0-or-later
// Included first in every file of a portable build (-DNXL_PORTABLE=ON, cmake/portable.cmake), so the programs run on
// Linux distributions with glibc 2.34 or newer (2021 on) whatever glibc they are built with:
//   · glibc 2.38's headers send strtol, sscanf ... to new C23 versions (__isoc23_*): that is turned off here;
//   · strlcpy and strlcat are glibc 2.38's: these use inline versions of their own;
//   · fmod, fmodf (2.38) and hypotf (2.35) have new symbol versions: the old ones are linked.
// glibc_compat.c gives the few C23 functions and arc4random (2.36) the static C++ runtime and the recompiler call.
#pragma once
#if !defined(__cplusplus) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE   // (the files define it themselves: it must be set before the first system header, which is now this)
#endif
#include <features.h>
#undef __GLIBC_USE_C2X_STRTOL
#define __GLIBC_USE_C2X_STRTOL 0
#undef __GLIBC_USE_C23_STRTOL
#define __GLIBC_USE_C23_STRTOL 0
#include <stddef.h>
#include <string.h>
static inline size_t nxl_strlcpy(char *d, const char *s, size_t n) {
    size_t l = strlen(s);
    if (n) { size_t k = l < n - 1 ? l : n - 1; memcpy(d, s, k); d[k] = 0; }
    return l;
}
static inline size_t nxl_strlcat(char *d, const char *s, size_t n) {
    size_t dl = strnlen(d, n);
    return dl == n ? n + strlen(s) : dl + nxl_strlcpy(d + dl, s, n - dl);
}
#define strlcpy nxl_strlcpy
#define strlcat nxl_strlcat
__asm__(".symver fmod,fmod@GLIBC_2.2.5");
__asm__(".symver fmodf,fmodf@GLIBC_2.2.5");
__asm__(".symver hypotf,hypotf@GLIBC_2.2.5");
