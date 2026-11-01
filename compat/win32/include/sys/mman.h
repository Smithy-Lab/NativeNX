// SPDX-License-Identifier: GPL-3.0-or-later
// mmap, munmap and mprotect on Windows (src/compat_win32.c), for the anonymous mappings nxlinux and nxnative's host
// code make: VirtualAlloc reserves and commits, VirtualFree gives back. Mapping a file is not supported (MAP_FAILED,
// ENOTSUP); nothing built for Windows maps one.
#pragma once
#include <stddef.h>
#include <sys/types.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_FIXED_NOREPLACE 0x100000
#define MAP_FAILED ((void *)-1)
#define MADV_DONTNEED 4

// with no address: anywhere; PROT_NONE only reserves (committed by a later mprotect, as the pages are used). MAP_FIXED
// inside a reservation: those pages given back and committed anew (zero); outside one: a new mapping there
void *mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off);
// a whole mapping given back; part of one: its pages given back, the range left reserved
int munmap(void *addr, size_t len);
// pages reserved but not committed are committed as they are given access
int mprotect(void *addr, size_t len, int prot);
int madvise(void *addr, size_t len, int advice);
