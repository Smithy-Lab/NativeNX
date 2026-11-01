# How nxlinux works

## The idea

A Switch game is ARM64 code that talks to Horizon (the Switch's kernel) through supervisor calls, and to Nintendo's
SDK through ordinary function calls. nxnative showed that a game does not need an emulator: its own code can run
natively, as long as the host answers those calls the way Horizon and the SDK would, and replaces the SDK's graphics
API (NVN) with the host's. That works on Apple silicon because the CPU is ARM64 too.

On a PC the CPU is x86_64. nxlinux keeps nxnative's model and changes one thing: the game's code is recompiled to
x86_64 before it runs.

```
               nxnative (Mac)                              nxlinux (PC)
  game ARM64 code ── runs directly ──┐       game ARM64 code ── recompiled to x86_64 (ScarletNX) ──┐
                                      ▼                                                               ▼
              host: SVCs, SDK services, NVN on Metal       host: SVCs, SDK services (nxnative's code), NVN on Vulkan
```

## One address space

Guest address `a` is host address `a`. The recompiler's CPU state has a base pointer for guest memory (`CPU.mem`), and
nxlinux sets it to 0. The guest's regions (`src/memory.c`) are reserved at their Horizon addresses: modules from 128
MiB, the alias, heap and stack regions above, all below 512 GiB, where an x86_64 Linux process has nothing mapped.

This is what lets nxnative's host code work unchanged. That code takes guest pointers and dereferences them, writes
results into the game's structures, and stores host objects keyed by guest addresses. With an offset between the two
spaces, every one of those sites would need a translation. With identity mapping, none do.

The game's code is never executed in place: only the recompiled copy runs. Its text and read-only data are mapped
read-only and nothing in the guest's image is executable on the host.

## Loading (src/loader.c)

An exefs holds `main`, `subsdk0`-`9` and `sdk` as NSO files. Each is decompressed into the ASLR region. When all are
mapped, every module's relocations are applied (`RELATIVE`, `GLOB_DAT`, `JUMP_SLOT`, `ABS64`, and `DT_RELR` for SDK 20
and later), binding imports the way nxnative's loader does:

1. a forced host implementation (`HLE_FORCE`) wins over everything;
2. then a definition in any loaded module, in load order;
3. then a plain host implementation (`HLE`);
4. otherwise a stub that stops the program with the import's name if it is ever called.

`rtld` is not loaded: the loader does its work. For a retail title, `main.c` then runs the SDK's start-up sequence
(`_init_libc0`, `nnosInitialize`, `nninitInitializeSdkModule`, `nninitStartup`, each module's `DT_INIT`, `nnMain`) as
nxnative's `dm/host/main.c` does. Before linking, the loader applies nxnative's registered cuts for the module's build,
a game's frame step stubs (60 fps), and the mods' code patches (`src/exefs_patch.c`: IPS and pchtxt).

## Recompiling (src/cpu.c)

The text of every module goes to ScarletNX's `snx_recompile_ranges` as one unit, with every exported function and init
entry as a starting point. The result is one dispatch table over all modules. Code the ahead-of-time pass misses is
compiled on first use by the recompiler's JIT. Compiled code is cached on disk by a hash of its inputs
(`~/.cache/nxlinux/<level>`, a folder per recompiler patch level).

Each guest thread is a host thread with its own `CPU` state bound to that unit. Guest functions become host functions,
so guest call depth is host stack depth: guest threads get 64 MiB host stacks (reserved, committed as used).

## Crossing between guest and host

**Guest calls host.** Every host implementation gets a stub address in a reserved, never-mapped region. Imports bind to
those addresses. When recompiled code branches there, there is no guest code to run, so the recompiler calls its miss
hook. nxlinux's hook looks up the stub, runs its thunk, and returns to the guest's `x30`.

The thunks are generated (`tools/gen_thunks.py`). The generator reads each `HLE(...)` / `HLE_FORCE(...)` declaration (the
same macros nxnative uses), and writes a thunk that takes each parameter from the guest registers by the AAPCS64 rules
(`src/hle_abi.h`: x0-x7, v0-v7, then the stack; 9-16 byte composites in two registers, larger ones by reference) and
writes the result back (x0/x1, v0, or the x8 buffer).

**Host calls guest.** `GUEST_CALL(fn, args...)` runs a guest function on the calling thread's own CPU: the state is saved,
the function runs until it returns to a sentinel address, and the state is restored. Nesting works
(guest → host → guest → host), which callbacks such as `qsort`'s comparator need.

nxnative's code goes further: it calls guest functions through plain C function pointers obtained from `guest_sym`.
nxlinux supports that too. For a guest function, `host_callable` returns a 22-byte x86_64 stub that loads the guest
address and jumps to `tramp.S`. That code maps the System V arguments onto AAPCS64 registers (rdi..r9 and two stack slots
to x0-x7, xmm0-7 to v0-v7), runs the function, and returns x0/x1 in rax/rdx and v0 in xmm0. If such a stub address
reaches guest code (stored in a game structure, say), the miss hook recognises it and continues at the guest function.
On Windows `tramp_win64.S` does the same for the Microsoft x64 convention, which passes the first four arguments by
position: integer-only and floating-point-only calls map exactly, a mix of the two or a 16-byte result needs
`GUEST_CALL` there (docs/WINDOWS.md).

## The kernel (src/svc.c, src/sync.c)

SVCs follow Horizon's register ABI as libnx and the SDK's wrappers use it. Guest mutexes are words in guest memory:
the guest takes and releases them with its own atomics, and calls `ArbitrateLock` / `ArbitrateUnlock` only to wait or
hand over. Condition variables release the mutex while waiting and take it back on wake-up, following Atmosphère's
mesosphère. All waits use one lock and one condition variable, which is simple and correct; per-address queues are on
the roadmap.

## nxnative's sources (compat/nxnative)

A service file from nxnative's `dm/host` is compiled as is:

- its `#include "hle.h"` finds `compat/nxnative/hle.h`, which defines the same macros for nxlinux's thunk generator;
- its `#include "dm.h"` finds nxnative's own `dm.h`, and `game_profile.h` is generated by nxnative's own
  `dm/games/game.py` (or a neutral default without a game);
- `guest_sym` is redirected to a version that returns native stubs for functions;
- `compat/nxnative/compat.c` implements the host API those files call that nxnative provides from Mac-specific files:
  clocks, entropy, the guest-object map, and service SystemEvents (bound through the guest SDK's own
  `nn::os::CreateSystemEvent`, as nxnative does).

The generator copies the source text into its output rather than `#include`-ing it. C resolves a quoted include from
the including file's own folder first, which would find nxnative's Mac headers instead of the compat ones.

## Graphics (gfx/)

NVN, the Switch's graphics API, is nxnative's code; it records the game's commands into lists. `gfx/vk_draw.c` plays
those lists back as Vulkan: draw state into pipelines (kept in a hash table and in a pipeline cache file), textures and
samplers (`vk_tex.c`: the game's memory imported as Vulkan buffers where it can be, ASTC decoded on the CPU when the GPU
has no ASTC), shaders translated from the Switch's GPU code to SPIR-V (`nxl_shader.cpp`, with ScarletNX's translator).
`vk_present.c` shows the frames in an SDL window (FSR 1.0 upscaling in `vk_upscale.c`, NativeNX's menu over the game in
`vk_overlay.c`); `vk_core.c` holds the device, memory pools, submissions and fences. Without Vulkan, the null backend
(`compat/nxnative/nvn_null.c`) runs the game with nothing drawn.

A game that brings its own OpenGL or Vulkan driver (`loader_is_gpu_driver`) has its calls bridged to the PC's driver
instead: `tools/gen_gfxapi.py` makes a bridge for each function in `gfx/api/*.sigs` (from the Khronos registries), and
`hle_egl.c` / `hle_vk.c` do the window-system parts (an EGL pbuffer read back into the window; a Vulkan surface on it).

## Files, sound, movies, input

- Files: nxnative's file service (`hle_fs.c`) reads the game's extracted RomFS folder (`DM_ROMFS`), its DLC's
  (`DM_AOC_DIR/<index>/`) and mods' folders laid over them; nxlinux reads no dump files and decrypts nothing.
- `compat/nxnative/pcmout.c`, `hle_audout.c`: sound through SDL (the audio renderer is nxnative's).
- `compat/nxnative/movie_codec.c`: H.264, VP8 / VP9 and AAC through the PC's FFmpeg, loaded when a movie plays.
- `gfx/nxl_input.c`: SDL controllers, keyboard and vibration as the Switch's.

## NativeNX (launcher/)

The dashboard is a separate program that starts nxlinux. Its parts are listed in `launcher/nativenx.h`: drawing
(`ui_draw.c`), input (`input.c`), the screens (`dashboard.c`, `settings.c`, `profiles_ui.c`, `news_album.c`,
`achievements.c`), starting a game (`launch.c`), the menu over a running game (`home_menu.c`) and the main loop
(`main.c`). Beside them: the games folder (`library.c`), profiles (`profile.c`), achievements (`achieve.c`), and BotW's
BCML mods (`bnp.c`, `aamp.c`). The game's window and NativeNX talk through two pipes (pop-ups and the menu one way,
the Home button and keys the other).

## Tests

`tests/guest` holds ARM64 programs compiled with clang for the Switch's Cortex-A57 and linked by lld into the NSO
layout (`guest.ld`, `crt0.S` with a MOD0 header). `tools/elf2nso` packs them as real NSO files, so the tests go through
the same loader as games. Each program prints `ok` / `not ok` lines and a summary that CTest checks.

`tests/launcher`, `tests/gfx` and `tests/exefs` test the launcher (driven by scripted presses), graphics without a GPU
(Mesa's lavapipe) and the mods' code patches. `docs/DEVELOPING.md` says how to run them, with the sanitizers, and the parsers' fuzzer.

## Linux and Windows

The two systems share nearly all the code. What differs sits behind two small interfaces: `src/platform.h` (what
nxlinux and NativeNX both need: starting and watching a program, pipes between them, the user's folders) and
`src/host.h` (nxlinux's: the guest's memory at fixed addresses, the fault handler, samples of where a thread is, CPU
times, backtraces), each with a `_posix.c` and a `_win32.c`. On Windows `src/compat_win32.h`, force-included in every
file, and `compat/win32/include` fill in the POSIX calls the rest uses (`mmap` over `VirtualAlloc`, `dlopen` over
`LoadLibrary`, `dirent.h` with entry types ...), and the build (cmake/windows.cmake) links the C runtime's files as
binary and gives the programs a manifest that makes UTF-8 their code page, so names stay UTF-8 as on Linux. The guest's
`long` is 64 bits and Windows' 32: the thunk generator moves a `long` argument or result as 64 bits.
