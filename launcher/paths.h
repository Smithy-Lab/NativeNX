// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's folders: settings in ~/.config/nxlinux, its data (logs, the album, avatars, your sounds) in
// ~/.local/share/nxlinux, as the XDG base directories say (XDG_CONFIG_HOME, XDG_DATA_HOME); merged mods in
// ~/.cache/nxlinux; pictures handed to the game's window in XDG_RUNTIME_DIR (else /tmp). On Windows: %APPDATA%\nxlinux,
// %LOCALAPPDATA%\nxlinux and %TEMP% (src/platform_win32.c)
#pragma once
#include <stddef.h>

// ~/.config/nxlinux/<sub> (data 0) or ~/.local/share/nxlinux/<sub> (data 1); sub "" for the folder itself
void nx_dir(int data, const char *sub, char *out, size_t n);
// ~/.cache/nxlinux/<sub>
void nx_cache_dir(const char *sub, char *out, size_t n);
// ~/.local/share itself (the desktop's own: applications, icons)
void nx_data_home(char *out, size_t n);
// the folder and the ones above it
void nx_mkdirs(const char *path);
const char *nx_runtime_dir(void);
