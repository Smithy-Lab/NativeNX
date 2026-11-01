// SPDX-License-Identifier: GPL-3.0-or-later
// getrandom on Windows: the system's generator (BCryptGenRandom, src/compat_win32.c)
#pragma once
#include <stddef.h>
#include <sys/types.h>
ssize_t getrandom(void *buf, size_t n, unsigned flags);
