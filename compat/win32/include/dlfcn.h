// SPDX-License-Identifier: GPL-3.0-or-later
// dlopen and dlsym on Windows: LoadLibrary and GetProcAddress (src/compat_win32.c)
#pragma once
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
