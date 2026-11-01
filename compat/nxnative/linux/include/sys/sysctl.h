// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin's sysctlbyname (hardware queries such as hw.nperflevels): not available; nxnative falls back to its defaults
#pragma once
#include <errno.h>
#include <stddef.h>
static inline int sysctlbyname(const char *name, void *old, size_t *oldlen, void *new_, size_t newlen) {
    (void)name; (void)old; (void)oldlen; (void)new_; (void)newlen;
    errno = ENOENT;
    return -1;
}
