// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's folders (paths.h), from the system's own (src/platform.h)
#include "paths.h"
#include "platform.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0755)
#endif

void nx_data_home(char *out, size_t n) { nxl_desktop_data_dir(out, n); }
void nx_dir(int data, const char *sub, char *out, size_t n) {
    char base[1024];
    nxl_user_dir(data ? NXL_DIR_DATA : NXL_DIR_CONFIG, base, sizeof base);
    if (sub && *sub) snprintf(out, n, "%s/%s", base, sub); else snprintf(out, n, "%s", base);
}
void nx_cache_dir(const char *sub, char *out, size_t n) {
    char base[1024];
    nxl_user_dir(NXL_DIR_CACHE, base, sizeof base);
    snprintf(out, n, "%s/%s", base, sub);
}
void nx_mkdirs(const char *path) {
    char t[1200];
    snprintf(t, sizeof t, "%s", path);
    // (on Windows "C:/..." starts with a drive, which is there)
    for (char *q = t + 1; *q; q++) if (*q == '/' || *q == '\\') { char c = *q; *q = 0; if (q[-1] != ':') mkdir_one(t); *q = c; }
    mkdir_one(t);
}
const char *nx_runtime_dir(void) {
    static char d[1024];
    if (!d[0]) nxl_user_dir(NXL_DIR_RUNTIME, d, sizeof d);
    return d;
}
