// SPDX-License-Identifier: GPL-3.0-or-later
// Darwin's clonefile and renamex_np on Windows (compat/nxnative/compat_win32.c): a file or a whole folder copied (its
// attribute streams with it), and two folders swapped by renaming (not in one step, as on the Mac and Linux: nxnative's
// transactional saves are copied whole, then swapped)
#pragma once
#define CLONE_NOFOLLOW 0x0001
#define RENAME_SWAP 0x2
int clonefile(const char *src, const char *dst, unsigned flags);
int renamex_np(const char *from, const char *to, unsigned flags);
