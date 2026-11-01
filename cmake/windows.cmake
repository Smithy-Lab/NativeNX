# SPDX-License-Identifier: GPL-3.0-or-later
# Windows x86_64, with MinGW-w64 and the UCRT (llvm-mingw, or MSYS2's CLANG64): what differs from Linux, set before the
# rest of CMakeLists.txt (whose header lists the variables). docs/WINDOWS.md says how to build and what runs there.
#   src/compat_win32.h       the smaller POSIX calls, included first in every C and C++ file
#   compat/win32/include     POSIX headers Windows lacks (sys/mman.h, dlfcn.h, dirent.h with d_type, elf.h)
#   src/compat_win32.c, src/platform_win32.c   their implementations and platform.h's, in every program (nxl_win32)
#   src/host_win32.c         host.h: VirtualAlloc, exception handlers; src/tramp_win64.S: the Microsoft x64 trampolines
#   compat/nxnative/win32    nxnative's Darwin calls on Windows (attribute streams, clonefile as a copy)
set(NXL_WIN32_CFLAGS -include ${CMAKE_CURRENT_SOURCE_DIR}/src/compat_win32.h -I${CMAKE_CURRENT_SOURCE_DIR}/compat/win32/include
                     -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -D__USE_MINGW_ANSI_STDIO=1)
add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/src/compat_win32.h>")
include_directories(compat/win32/include)
add_compile_definitions(_WIN32_WINNT=0x0A00 WINVER=0x0A00 __USE_MINGW_ANSI_STDIO=1)
# no DLLs of the compiler's (libc++, libunwind, winpthread) to carry: only SDL3.dll beside the programs
add_link_options(-static)
# files are binary, as on Linux (no CR LF turned into LF on reading): MinGW's binmode.o sets that when it is linked in
execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=binmode.o OUTPUT_VARIABLE NXL_BINMODE OUTPUT_STRIP_TRAILING_WHITESPACE)
if(EXISTS "${NXL_BINMODE}")
    add_link_options(${NXL_BINMODE})
else()
    message(FATAL_ERROR "MinGW's binmode.o was not found (${NXL_BINMODE}): files would be read as text")
endif()
enable_language(RC)

# src/platform.h and the rest of src/compat_win32.h, linked into every program
add_library(nxl_win32 STATIC src/platform_win32.c src/compat_win32.c)
target_link_libraries(nxl_win32 PUBLIC ole32 uuid shell32 psapi bcrypt pthread)
link_libraries(nxl_win32)

set(NXL_PLATFORM_SOURCES)   # (nxl_win32)
set(NXL_HOST_SOURCES src/host_win32.c src/tramp_win64.S src/sections_win32.c packaging/windows/nxlinux.rc)
set(NXL_NXNATIVE_HOST_SOURCES compat/nxnative/compat_win32.c)
set(NXL_GEN_FLAGS ${NXL_WIN32_CFLAGS})
set(NXL_SHIM_FLAGS -include ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative/linux/linux_shim.h)
set(NXL_SHIM_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative/win32/include ${CMAKE_CURRENT_SOURCE_DIR}/compat/win32/include
                      ${CMAKE_CURRENT_SOURCE_DIR}/compat/nxnative/linux/include)
set(NXL_SYS_LIBS ws2_32 winmm)
set(NXL_NXNATIVE_EXTRA_FLAGS -Wno-deprecated-declarations)   # (nxnative reads tzname, which the UCRT marks deprecated)
# a zip's layout: the programs, their files beside them (src/platform.h finds them there)
set(NXL_BIN_DIR .)
set(NXL_SHARE_DIR .)
set(NXL_LAUNCHER_DIR data)

# the programs' icon (made from NativeNX's), version and manifest (UTF-8 as their code page, Windows 10 and newer,
# sharp on high-DPI screens): packaging/windows/*.rc
set(NXL_ICO ${CMAKE_CURRENT_BINARY_DIR}/nativenx.ico)
add_custom_command(OUTPUT ${NXL_ICO}
                   COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/png2ico.py ${CMAKE_CURRENT_SOURCE_DIR}/launcher/data/nativenx-icon.png ${NXL_ICO}
                   DEPENDS packaging/windows/png2ico.py launcher/data/nativenx-icon.png
                   COMMENT "Making the Windows icon")
set_source_files_properties(packaging/windows/nativenx.rc packaging/windows/nxlinux.rc packaging/windows/manifest.rc PROPERTIES
                            OBJECT_DEPENDS "${NXL_ICO};${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/nxlinux.manifest"
                            INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows")
# LLVM's CMake package (LLVM_DIR), so the LLVM libraries an installed recompiler names are its targets, each with the
# libraries it needs in turn (a static LLVM on Windows has no llvm-config that runs on the build machine to list them)
find_package(LLVM CONFIG QUIET)
