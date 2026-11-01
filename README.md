# nxlinux + NativeNX

**Run your own Nintendo Switch games natively on a PC (Linux and Windows).**

> ⚠️ **This project is 100% vibe coded.** Every line was written by an AI (Claude) from a person's instructions. It works
> on the setups it was tested on, but expect rough edges.
>
> 🚫 **It will not be maintained.** No updates, fixes, issues or pull requests will be handled. Fork it if you want to
> take it further.
>
> ✅ **It is free to use.** Use it, change it and share it under the GPL-3.0 licence (see `LICENSE`, and `NOTICE.md` for
> the libraries it includes).

## What it is

- **nxlinux** runs a Switch game's own ARM64 code on an x86_64 PC. The code is recompiled to x86_64 ahead of time by
  [ScarletNX](https://github.com/scarletnx/ScarletNX)'s recompiler and runs against a host layer that stands in for the
  Switch's system, built on [nxnative](https://github.com/seedds/nxnative)'s sources.
- **NativeNX** is a console-style launcher for it: a dashboard of your games, controller support, profiles,
  achievements, screenshots, mods (including BCML mods for Breath of the Wild) and a Home menu over the running game.
- Graphics run on Vulkan, with FSR upscaling, resolution scaling and 60 fps for some games. Sound and controllers go
  through SDL3.

**No games, keys or Nintendo files are included, and nothing is decrypted.** nxlinux only runs game files you have
already extracted from your own console. It cannot read `.xci` / `.nsp` files or keys.

This is not affiliated with or endorsed by Nintendo. Nintendo Switch is a trademark of Nintendo.

## Requirements

- A 64-bit x86 PC with a CPU that supports AVX2
- A GPU with a Vulkan driver (NVIDIA, AMD or Intel)
- **Linux:** any distribution from 2021 or newer (glibc 2.34+: Ubuntu 22.04, Debian 12, Fedora 35, Arch, SteamOS 3 ...)
- **Windows:** Windows 10 or 11 (the Windows version is new and has not been tested on a real Windows PC)

## How to use it

### 1. Download

- **Linux:** `NativeNX-x86_64.AppImage`. Make it executable (`chmod +x NativeNX-x86_64.AppImage`) and run it.
- **Windows:** `NativeNX-windows-x86_64.zip`. Unzip it anywhere and run `nativenx.exe`.

### 2. Set up your games

Each game is a folder of files extracted from **your own** console:

```
Games/
  My Game/
    ExeFS/      main, main.npdm, subsdk*, sdk   (with any update already applied)
    RomFS/      the game's files
    DLC/1/ ...  optional: each DLC's files in a folder named by its number
    mods/       optional: your mods, one folder each
```

### 3. Play

Start NativeNX, choose your games folder the first time, then pick a game and press **A** (or **Enter**).

- Each game's **Options** (**+** on a controller, **O** on the keyboard) set its graphics, frame rate, resolution and mods.
- **Settings** set the upscaling, full screen, sounds and profiles.
- In a game, **Home** (or **Shift+Tab**) opens the menu; **F11** switches full screen; **F12** takes a screenshot.

The first start of a game recompiles its code, which takes a few minutes. Later starts take seconds.

## More

- [docs/FEDORA.md](docs/FEDORA.md): the full user guide (controls, graphics settings, mods, 60 fps, troubleshooting)
- [docs/BUILDING.md](docs/BUILDING.md): building from source on Linux, and the AppImage
- [docs/WINDOWS.md](docs/WINDOWS.md): the Windows version: where things go, what differs, building it
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it works inside
- [docs/DEVELOPING.md](docs/DEVELOPING.md): the code's layout and the tests

## Credits and licence

GPL-3.0-or-later (it links ScarletNX's GPL-3.0 recompiler). It builds on:

- **[nxnative](https://github.com/seedds/nxnative)** (MIT): the native-execution approach, the host's design, its boot
  sequence and service implementations, and its game profiles.
- **[ScarletNX](https://github.com/scarletnx/ScarletNX)** (GPL-3.0): the ARM64-to-x86_64 recompiler.
- **[LZ4](https://github.com/lz4/lz4)** (BSD-2-Clause), yuzu's shader recompiler (GPL-2.0-or-later), FSR, stb and the
  other parts listed in `NOTICE.md`, each under its own licence.
- Through those projects: Ryujinx, yuzu, LibHac, Switchbrew and libnx's documentation of Horizon.
