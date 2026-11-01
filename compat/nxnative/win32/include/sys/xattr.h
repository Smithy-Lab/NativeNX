// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin's extended-attribute calls on Windows: NTFS keeps a named stream beside a file's data ("<path>:<name>"), which
// holds the attribute (nxnative's hle_fs.c keeps a file's guest name in one when two names differ only in case).
#pragma once
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#ifndef ENOATTR
#define ENOATTR ENODATA
#endif
#define XATTR_NOFOLLOW 0x0001
ssize_t nxl_getxattr(const char *path, const char *name, void *value, size_t size);
int nxl_setxattr(const char *path, const char *name, const void *value, size_t size);
int nxl_removexattr(const char *path, const char *name);
#define getxattr(p, n, v, s, pos, opt) nxl_getxattr(p, n, v, s)
#define setxattr(p, n, v, s, pos, opt) nxl_setxattr(p, n, v, s)
#define removexattr(p, n, opt) nxl_removexattr(p, n)
