# Working on nxlinux

## Where things are

| Folder | What it is |
| --- | --- |
| `src/` | nxlinux, the game runner: loading (`loader.c`), the recompiler glue and threads (`cpu.c`), memory (`memory.c`), the kernel's calls (`svc.c`, `sync.c`), host stand-ins for SDK functions (`hle*.c`); what differs between Linux and Windows behind `platform.h` and `host.h` (`*_posix.c`, `*_win32.c`) |
| `compat/win32/`, `cmake/windows*.cmake`, `packaging/windows/` | the Windows build ([WINDOWS.md](WINDOWS.md)) |
| `gfx/` | NVN on Vulkan, the window, upscaling, the overlay, controllers, the OpenGL / Vulkan bridges |
| `compat/nxnative/` | what nxnative's host sources need to build and run here, and the patches made to a few of them (`patches/`) |
| `launcher/` | NativeNX, the dashboard (its parts are listed in `launcher/nativenx.h`) |
| `tools/` | build-time generators (thunks, bridges, cuts, frame steps) and `elf2nso` |
| `tests/` | the tests (below) |
| `third_party/` | libraries built in (stb, LZ4, FSR, parts of a shader recompiler); `NOTICE.md` lists them and their licences |

`docs/ARCHITECTURE.md` explains how the pieces fit.

## Building and testing

    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build
    cd build && ctest

Without a GPU the graphics tests use Mesa's software Vulkan driver (lavapipe) when `-DNXL_TEST_VK_ICD=<its .json>`
points at it; they are skipped otherwise.

## The sanitizers

UndefinedBehaviorSanitizer works for everything:

    cmake -S . -B build-ubsan -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
          -DCMAKE_C_FLAGS="-fsanitize=undefined -fno-sanitize=function,vptr,alignment -fno-sanitize-recover=undefined -g -O1" \
          -DCMAKE_CXX_FLAGS="-fsanitize=undefined -fno-sanitize=function,vptr,alignment -fno-sanitize-recover=undefined -g -O1" \
          -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=undefined"
    cmake --build build-ubsan && (cd build-ubsan && ctest)

AddressSanitizer (`-fsanitize=address`, the same way) works for NativeNX and the unit tests, but not for nxlinux
itself: its shadow memory covers the addresses the Switch's code is loaded at ("cannot reserve the guest's ASLR region
... Something else is mapped there"). Run the tests that do not start nxlinux:

    ASAN_OPTIONS=detect_leaks=0 ctest -R "t_bnp|t_achieve|t_launcher$|t_gfxapi|t_input|t_shader|t_audio"

## Fuzzing the file parsers

Files from outside (mods) go through parsers that must not crash on a damaged one. `tests/fuzz/fuzz_parsers.c`
damages seed files at random and runs them through those parsers, with the sanitizers on:

    cmake -B build-asan -DNXL_FUZZ=ON ... && cmake --build build-asan --target fuzz_parsers
    ASAN_OPTIONS=detect_leaks=0 ./build-asan/fuzz_parsers aamp 200000 <some .bxml / .baslist files>

Modes: `aamp`, `sarc`, `yaz0`, `rstb`, `bnp` (a whole BCML mod, one of its files damaged, merged); the test folder
`t_bnp_work` makes good seeds. A crash stops it with a report,
and the input that caused it is in `/tmp/fuzz_parsers*.cur`.

## Style

C11 with GNU extensions, four spaces, lines up to about 160 columns. Every file starts with what it is for, and every
function longer than a few lines with what it does. Messages in the log say what happened in plain words, and
`[RED]` marks something that does not work as on the console (a missing function, a format not supported).
