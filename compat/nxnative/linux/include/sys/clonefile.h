// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin's clonefile (an APFS copy-on-write copy of a file or a whole directory tree) and renamex_np(RENAME_SWAP),
// which nxnative's hle_fs.c uses for transactional saves. On Linux: a recursive copy whose files are reflinks where the
// file system has them (btrfs, Fedora's default; XFS), else copied, with extended attributes; and renameat2's exchange.
#pragma once
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/xattr.h>
#define CLONE_NOFOLLOW 0x0001
#define RENAME_SWAP 0x2
static inline int nxl_copy_xattrs(const char *s, const char *d) {
    char names[4096];
    ssize_t n = llistxattr(s, names, sizeof names);
    for (ssize_t i = 0; n > 0 && i < n; i += (ssize_t)strlen(names + i) + 1) {
        char v[1024];
        ssize_t k = lgetxattr(s, names + i, v, sizeof v);
        if (k >= 0 && lsetxattr(d, names + i, v, (size_t)k, 0)) return -1;
    }
    return 0;
}
static inline int nxl_clone_file(const char *s, const char *d, mode_t mode) {
    int in = open(s, O_RDONLY | O_CLOEXEC), out = in < 0 ? -1 : open(d, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
    int r = in < 0 || out < 0 ? -1 : 0;
    if (!r && ioctl(out, FICLONE, in)) {   // no reflink here: copy the bytes
        char buf[1 << 16];
        ssize_t k;
        while ((k = read(in, buf, sizeof buf)) > 0)
            for (ssize_t w = 0; w < k;) { ssize_t x = write(out, buf + w, (size_t)(k - w)); if (x < 0) { r = -1; break; } w += x; }
        if (k < 0) r = -1;
    }
    int e = errno;
    if (in >= 0) close(in);
    if (out >= 0) close(out);
    errno = e;
    return r;
}
static inline int clonefile(const char *src, const char *dst, unsigned flags) {
    (void)flags;
    struct stat st;
    if (lstat(src, &st)) return -1;
    if (!access(dst, F_OK)) { errno = EEXIST; return -1; }
    if (S_ISLNK(st.st_mode)) {
        char t[4096];
        ssize_t n = readlink(src, t, sizeof t - 1);
        if (n < 0) return -1;
        t[n] = 0;
        return symlink(t, dst);
    }
    if (S_ISREG(st.st_mode)) return nxl_clone_file(src, dst, st.st_mode) || nxl_copy_xattrs(src, dst) ? -1 : 0;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTSUP; return -1; }
    if (mkdir(dst, st.st_mode & 07777)) return -1;
    DIR *d = opendir(src);
    if (!d) return -1;
    int r = 0;
    for (struct dirent *e; !r && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        size_t a = strlen(src) + strlen(e->d_name) + 2, b = strlen(dst) + strlen(e->d_name) + 2;
        char *s = malloc(a), *t = malloc(b);
        snprintf(s, a, "%s/%s", src, e->d_name);
        snprintf(t, b, "%s/%s", dst, e->d_name);
        r = clonefile(s, t, flags);
        free(s), free(t);
    }
    int e = errno;
    closedir(d);
    errno = e;
    return r ? r : nxl_copy_xattrs(src, dst);
}
static inline int renamex_np(const char *from, const char *to, unsigned flags) {
    return renameat2(AT_FDCWD, from, AT_FDCWD, to, flags & RENAME_SWAP ? RENAME_EXCHANGE : 0);
}
