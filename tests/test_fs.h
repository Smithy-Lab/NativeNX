// SPDX-License-Identifier: GPL-3.0-or-later
// Folders for the tests, made and removed without a shell (the same on Linux and Windows)
#pragma once
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef _WIN32
#include <direct.h>
#define tfs_mkdir1(p) _mkdir(p)
#else
#define tfs_mkdir1(p) mkdir(p, 0755)
#endif

// the folder and the ones above it (mkdir -p)
static inline void tfs_mkdirs(const char *path) {
    char t[2048];
    snprintf(t, sizeof t, "%s", path);
    for (char *q = t + 1; *q; q++) if ((*q == '/' || *q == '\\') && q[-1] != ':') { char c = *q; *q = 0; tfs_mkdir1(t); *q = c; }
    tfs_mkdir1(t);
}
// the folder and everything in it (rm -rf); a link is removed, not followed
static inline void tfs_rmtree(const char *path) {
    struct stat st;
#ifdef _WIN32
    if (stat(path, &st)) return;
#else
    if (lstat(path, &st)) return;
#endif
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    DIR *d = opendir(path);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[2048];
        snprintf(p, sizeof p, "%s/%s", path, e->d_name);
        tfs_rmtree(p);
    }
    if (d) closedir(d);
    rmdir(path);
}
// a folder for the test's files: TMPDIR, TEMP on Windows, else /tmp
static inline const char *tfs_tmp(void) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = getenv("TEMP");
    return t && *t ? t : "/tmp";
}
