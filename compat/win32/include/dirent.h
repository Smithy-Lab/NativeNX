// SPDX-License-Identifier: GPL-3.0-or-later
// Folders listed on Windows (src/compat_win32.c), with what POSIX code there expects that MinGW's <dirent.h> lacks:
// UTF-8 names, an entry's type (d_type), dirfd and fstatat relative to the listed folder
#pragma once
#include <sys/stat.h>
#include <sys/types.h>

#define DT_UNKNOWN 0
#define DT_DIR 4
#define DT_REG 8
#define DT_LNK 10
struct dirent {
    ino_t d_ino;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[1024];
};
typedef struct nxl_dir DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
void rewinddir(DIR *d);
int closedir(DIR *d);
// a number for the folder, which fstatat takes (it is not a file descriptor: nothing else does)
int dirfd(DIR *d);
#define AT_FDCWD (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
int fstatat(int dirfd, const char *name, struct stat *st, int flags);
