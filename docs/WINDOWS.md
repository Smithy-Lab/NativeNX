# nxlinux and NativeNX on Windows

The Windows build is the same program as the Linux one, built for Windows 10 and 11 (x86_64, a CPU with AVX2) with a
Vulkan driver. It is new: the test programs pass under Wine, but games have not been tried on a Windows PC yet. Please
report what happens with yours, with its log (below).

## Using it

Unpack `NativeNX-windows-x86_64.zip` anywhere and start `nativenx.exe`. It works as on Linux: choose your games
folder in Settings, and it starts `nxlinux.exe` (beside it) for a game. Each game is a folder of its extracted files, as on Linux
(docs/FEDORA.md, "What a game folder holds").

| What                       | Where on Windows                                          |
|----------------------------|-----------------------------------------------------------|
| NativeNX's settings         | `%APPDATA%\nxlinux\`                                       |
| logs, the album, avatars    | `%LOCALAPPDATA%\nxlinux\` (a game's log: `logs\<game>.log`) |
| merged mods, compiled code  | `%LOCALAPPDATA%\nxlinux\cache\`                            |

Settings > About > "Add NativeNX to the app menu" puts it in the Start menu. Your GPU's driver from NVIDIA, AMD or
Intel brings Vulkan (`vulkan-1.dll`); nothing else needs installing.

## What differs from Linux

- Saves are copied whole on each commit and swapped by renaming (on Linux and the Mac the copy is a cheap clone). A
  file name that differs only in case from another in a save is kept in an NTFS stream beside it.
- Games that use OpenGL (EGL) rather than NVN or Vulkan do not start: there is no EGL on Windows.
- Host code that calls a game's function through a plain C pointer can pass only integer arguments, or only
  floating-point ones, and get back a result of one register (`src/tramp_win64.S`). Windows passes the first four
  arguments by position, so a pointer cannot tell which registers hold which. nxnative's code mostly calls through
  `GUEST_CALL`, which has no such limit. The guest test shows these two cases as skipped.
- A game's crash report names the recompiled function by the dispatch table rather than by symbol.

## Building it

From Linux, with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (UCRT, x86_64), the build needs these, built for
Windows into one prefix (here `$P`):

- LLVM 17 or newer (its static libraries; the targets `X86`, the components `core native passes target analysis`),
  configured with `-DLLVM_HOST_TRIPLE=x86_64-w64-windows-gnu` and a Linux `llvm-tblgen`
- ScarletNX's recompiler library: ScarletNX at the commit `CMakeLists.txt` names, patched with
  `tools/patch_scarletnx.py`, configured `-DSNX_TOOLS_ONLY=ON -DLLVM_DIR=$P/lib/cmake/llvm` (with the Linux build's
  `llvm-as` as `SNX_LLVM_AS` when the compiler is newer than that LLVM), and installed
- SDL3 (the `SDL3-devel-*-mingw.zip` of SDL's releases), the Vulkan headers and Vulkan-Loader (for its import library)

Then, with a CMake toolchain file for llvm-mingw (`CMAKE_SYSTEM_NAME Windows`, its `x86_64-w64-mingw32-clang`,
`CMAKE_FIND_ROOT_PATH` including `$P`):

    cmake -S . -B build-win -G Ninja -DCMAKE_TOOLCHAIN_FILE=mingw.cmake -DCMAKE_BUILD_TYPE=Release \
          -DLLVM_DIR=$P/lib/cmake/llvm -DScarletNXRecompiler_DIR=$P/lib/cmake/ScarletNXRecompiler \
          -DSDL3_DIR=$P/lib/cmake/SDL3 -DVulkan_INCLUDE_DIR=$P/include -DVulkan_LIBRARY=$P/lib/libvulkan-1.dll.a
    cmake --build build-win
    packaging/windows/make_zip.sh build-win $P/bin/SDL3.dll

On Windows itself the same works in MSYS2's CLANG64 shell, with its packages for LLVM, SDL3, Vulkan and CMake.

The tests that need no game (`ctest`, `cmake/windows_tests.cmake`) and the guest test programs run under Wine:

    wine build-win/nxlinux.exe --no-cache build/guest/t_basic.nso

(the guest programs are built by the Linux build: they are the same files for both).

## How the port is laid out

What differs between the systems is in a few files; the rest is shared:

- `src/platform.h` (`platform_posix.c`, `platform_win32.c`): starting and watching a program, pipes, the user's folders
- `src/host.h` (`host_posix.c`, `host_win32.c`): the guest's memory at fixed addresses, the fault handler, samples
  of where a thread is, CPU times, backtraces
- `src/compat_win32.h` and `compat/win32/include`: the POSIX calls and headers Windows lacks (`mmap`, `dlopen`,
  `dirent.h` with `d_type`, `fnmatch`, `elf.h` ...), `src/compat_win32.c` their implementations
- `compat/nxnative/win32`: nxnative's Darwin calls (file attributes, `clonefile`)
- `src/tramp_win64.S`: host-callable guest functions in the Microsoft x64 calling convention;
  `src/sections_win32.c`: the bounds of the tables the linker builds
- `cmake/windows.cmake`, `cmake/windows_tests.cmake`, `packaging/windows/`
