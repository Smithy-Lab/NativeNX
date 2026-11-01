# SPDX-License-Identifier: GPL-3.0-or-later
# -DNXL_PORTABLE=ON: programs that run on Linux distributions from 2021 on (glibc 2.34 and newer), for the AppImage
# (packaging/appimage.sh): the C++ runtime linked in, the glibc versions held back (portable/glibc_compat.h), only the
# libraries the programs use linked. The GPU driver, the Vulkan loader, zlib and SDL3 stay the system's (the AppImage
# brings SDL3 and a Vulkan loader for systems without them).
option(NXL_PORTABLE "Build for any Linux distribution with glibc 2.34 or newer (the AppImage)" OFF)
set(NXL_PORTABLE_SOURCES)
if(NXL_PORTABLE)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/portable/glibc_compat.h>")
    add_link_options("$<$<LINK_LANGUAGE:CXX>:-static-libstdc++>" -static-libgcc -Wl,--as-needed)
    set(NXL_PORTABLE_SOURCES ${CMAKE_CURRENT_LIST_DIR}/portable/glibc_compat.c)
endif()
