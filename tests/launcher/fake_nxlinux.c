// SPDX-License-Identifier: GPL-3.0-or-later
// Stands in for nxlinux in the launcher's tests where a shell script cannot (Windows): prints what it was given, says
// whether the pipes NativeNX made are there and work (NXL_LAUNCHER_FD is written to), and ends with status 3.
#include "platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    printf("args:");
    for (int i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    static const char *const vars[] = {"DM_ROMFS", "DM_SAVE", "DM_AOC_DIR", "NXL_SEGMENT", "DM_ROMFS_OVERLAY", "NXL_ANISO", "DM_FPS_TARGET", "NXL_FPS",
                                       "NXL_RES_SCALE", "NXL_EXEFS_PATCHES", "NXL_FILL", "NXL_FULLSCREEN", "NXL_UPSCALE", "NXL_SHARPNESS", NULL};
    for (int i = 0; vars[i]; i++) printf("%s=%s\n", vars[i], getenv(vars[i]) ? getenv(vars[i]) : "");
    printf("NXL_NO_CUTS=%s\n", getenv("NXL_NO_CUTS") ? getenv("NXL_NO_CUTS") : "unset");
    int to = nxl_fd_from_env("NXL_NOTIFY_FD"), from = nxl_fd_from_env("NXL_LAUNCHER_FD");
    printf("pipes: from NativeNX %s, to it %s\n", to >= 0 ? "yes" : "no", from >= 0 && write(from, "hello\n", 6) == 6 ? "written" : "no");
    fflush(stdout);
    return 3;
}
