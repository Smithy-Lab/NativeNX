// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin's extended-attribute calls (a position and an options argument; any name) on Linux's (neither; names in a
// namespace): nxnative's hle_fs.c keeps a file's guest name in an attribute, which Linux stores as "user.<name>".
#pragma once
#include_next <sys/xattr.h>
#include <errno.h>
#include <stdio.h>
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define XATTR_NOFOLLOW 0x0001
static inline const char *nxl_xattr_name(const char *n, char *buf, size_t cap) { snprintf(buf, cap, "user.%s", n); return buf; }
static inline ssize_t nxl_getxattr(const char *p, const char *n, void *v, size_t s, unsigned pos, int opt) {
    char b[256]; (void)pos;
    return opt & XATTR_NOFOLLOW ? lgetxattr(p, nxl_xattr_name(n, b, sizeof b), v, s) : getxattr(p, nxl_xattr_name(n, b, sizeof b), v, s);
}
static inline int nxl_setxattr(const char *p, const char *n, const void *v, size_t s, unsigned pos, int opt) {
    char b[256]; (void)pos;
    return opt & XATTR_NOFOLLOW ? lsetxattr(p, nxl_xattr_name(n, b, sizeof b), v, s, 0) : setxattr(p, nxl_xattr_name(n, b, sizeof b), v, s, 0);
}
static inline int nxl_removexattr(const char *p, const char *n, int opt) {
    char b[256];
    return opt & XATTR_NOFOLLOW ? lremovexattr(p, nxl_xattr_name(n, b, sizeof b)) : removexattr(p, nxl_xattr_name(n, b, sizeof b));
}
#define getxattr(p, n, v, s, pos, opt) nxl_getxattr(p, n, v, s, pos, opt)
#define setxattr(p, n, v, s, pos, opt) nxl_setxattr(p, n, v, s, pos, opt)
#define removexattr(p, n, opt) nxl_removexattr(p, n, opt)
