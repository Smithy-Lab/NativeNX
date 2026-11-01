// SPDX-License-Identifier: GPL-3.0-or-later
// What differs between the systems nxlinux and NativeNX build for: src/platform_posix.c (Linux), and on Windows
// src/platform_win32.c. Only what more than one part needs is here; a part that is one system's only (the recompiler's
// memory, signals) keeps its own. On Windows, src/compat_win32.h (included first by every file there) fills in the
// smaller POSIX calls (mkdir with a mode, setenv, realpath ...).
#pragma once
#include <stddef.h>
#include <stdint.h>

// The folder the running program is in (no trailing separator, '/' between folders); 0 when found
int nxl_exe_dir(char *out, size_t n);
// A file of the program's own: beside it, else in <prefix>/share/<sub>/ (an install: bin/ and share/ side by side);
// the path in out, 0 when it is there
int nxl_data_file(const char *sub, const char *name, char *out, size_t n);

// ---- another program started and watched (NativeNX starting nxlinux) ----
// A started program: its process ID on Linux, its process handle on Windows; 0 for none.
typedef intptr_t nxl_proc;
typedef struct {
    const char *exe;
    const char *const *argv;   // argv[0] first, NULL at the end
    const char *log;           // its output and errors into this file (created anew); NULL: NativeNX's own
    const char *const *env;    // changes to its environment: "NAME=value" set, "NAME" removed; NULL at the end
    // Pipes to and from it, made when the name is given: the program finds its end's number in that environment
    // variable (nxl_fd_from_env), and to_fd / from_fd get NativeNX's end, which never waits (nothing to read: -1;
    // a full pipe: nothing written). -1 when there is none.
    const char *to_env, *from_env;
    int to_fd, from_fd;
} nxl_spawn_t;
nxl_proc nxl_spawn(nxl_spawn_t *s);
// 1 once it has ended (status: its exit code, or minus the signal that ended it), 0 while it runs
int nxl_proc_ended(nxl_proc p, int *status);
enum { NXL_PROC_STOP, NXL_PROC_CONT, NXL_PROC_TERM };
void nxl_proc_signal(nxl_proc p, int what);   // paused (the game at NativeNX's Home), resumed, asked to end
// in the started program: the descriptor of its end of a pipe (nxl_spawn_t's to_env / from_env), -1 without one
int nxl_fd_from_env(const char *name);

// A windowed program's output (stdout, stderr): Windows gives it none, so it goes to the console it was started from,
// else to <data>/logs/<name>.log; nothing to do on Linux
void nxl_console_attach(void);

// A folder opened in the desktop's file manager (nothing waits for it)
void nxl_open_folder(const char *dir);

// The user's folders, with nxlinux's own folder inside them (out: "<base>/nxlinux"):
//   Linux:   ~/.config, ~/.local/share, ~/.cache (or XDG_CONFIG_HOME, XDG_DATA_HOME, XDG_CACHE_HOME), and for files
//            passed to another program XDG_RUNTIME_DIR (else /tmp, with no nxlinux folder)
//   Windows: %APPDATA% for the settings, %LOCALAPPDATA% for the data and the cache (cache\ inside), %TEMP%
enum { NXL_DIR_CONFIG, NXL_DIR_DATA, NXL_DIR_CACHE, NXL_DIR_RUNTIME };
void nxl_user_dir(int kind, char *out, size_t n);
// The user's home folder: HOME (Windows: USERPROFILE); "" when there is none
void nxl_home_dir(char *out, size_t n);
// A descriptor that never waits (a pipe's end read or written while there is nothing to read or no room); 0 when set
int nxl_fd_nonblock(int fd);
// The desktop's own data folder (~/.local/share: its applications and icons); Windows: %LOCALAPPDATA%
void nxl_desktop_data_dir(char *out, size_t n);

// Between the folders of a list in one environment variable (DM_ROMFS_OVERLAY, NXL_EXEFS_PATCHES): ':' as in PATH,
// ';' on Windows (C:\...)
#ifdef _WIN32
#define NXL_LIST_SEP ";"
#else
#define NXL_LIST_SEP ":"
#endif

#ifdef _WIN32
// A Windows shortcut (.lnk) at lnk starting exe (its folder the working one); 0 when made
int nxl_shortcut(const char *exe, const char *lnk, const char *description);
#endif

// The name programs end with: ".exe" on Windows
#ifdef _WIN32
#define NXL_EXE_SUFFIX ".exe"
#else
#define NXL_EXE_SUFFIX ""
#endif
