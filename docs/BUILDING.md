# Building nxlinux and NativeNX

The easiest way to run them is the AppImage (below): one file that runs on any x86_64 Linux with glibc 2.34 or newer
and a Vulkan driver. Build from source to work on them, or for a distribution package. For Windows see
[WINDOWS.md](WINDOWS.md).

What every build needs: an x86_64 CPU with AVX2, Clang, CMake 3.25 or newer, Ninja, Python 3, LLVM 17 or newer (its
development files: the recompiler generates code with them), SDL3, the Vulkan headers and loader, and for the button
icons in game text Python's fontTools. The first configure fetches ScarletNX and nxnative from GitHub at pinned commits.

## Packages by distribution

**Fedora 42 and newer**

    sudo dnf install clang lld cmake ninja-build llvm-devel python3 git zlib-ng-compat-devel libzstd-devel \
        vulkan-loader-devel vulkan-headers SDL3-devel fmt-devel boost-devel spirv-headers-devel python3-fonttools glslang

**Arch Linux, Manjaro, EndeavourOS, SteamOS (developer mode)**

    sudo pacman -S --needed clang lld cmake ninja llvm python git zlib zstd vulkan-icd-loader vulkan-headers sdl3 \
        fmt boost spirv-headers python-fonttools glslang

**Debian 13, Ubuntu 25.04 and newer**

    sudo apt install clang lld cmake ninja-build llvm-dev python3 git zlib1g-dev libzstd-dev libvulkan-dev \
        libsdl3-dev libfmt-dev libboost-dev spirv-headers python3-fonttools glslang-tools

**Ubuntu 24.04, Linux Mint 22:** as above, with `llvm-18-dev` for `llvm-dev`, and SDL3 built from source (it is not
packaged there): `git clone -b release-3.2.24 https://github.com/libsdl-org/SDL && cmake -S SDL -B SDL/build -G Ninja
&& cmake --build SDL/build && sudo cmake --install SDL/build` (install `libpipewire-0.3-dev libpulse-dev
libasound2-dev libwayland-dev libxkbcommon-dev libdecor-0-dev libudev-dev libdbus-1-dev libx11-dev libxext-dev
libxrandr-dev libxcursor-dev libxi-dev libxss-dev libdrm-dev libgbm-dev` first so it has sound and Wayland).

**openSUSE Tumbleweed**

    sudo zypper install clang lld cmake ninja llvm-devel python3 git zlib-devel libzstd-devel vulkan-devel \
        SDL3-devel fmt-devel boost-devel spirv-headers python3-fonttools glslang-devel

**Older distributions** (Ubuntu 22.04, Debian 12): use the AppImage, or build with LLVM's own release
(`https://github.com/llvm/llvm-project/releases`, `-DCMAKE_PREFIX_PATH=<its folder>` and its `clang`), CMake from
`pip install cmake`, and SDL3 from source as for Ubuntu 24.04.

Your GPU needs a Vulkan driver to run games: Mesa's (`mesa-vulkan-drivers`, `vulkan-radeon`, `vulkan-intel`; NVK for
NVIDIA) or NVIDIA's own.

## Build

    git clone <this repository> nxlinux && cd nxlinux
    cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build
    ./build/nativenx

`ctest --test-dir build` runs the tests. `sudo cmake --install build` (or `--prefix ~/.local`) installs `nxlinux` and
`nativenx` into `bin/` with their files in `share/nxlinux` and `share/nativenx`; NativeNX's Settings > About adds it to
the desktop's app menu.

## The AppImage

A portable build holds back the glibc versions it uses (`cmake/portable/`), links the C++ runtime in and only the
libraries it needs, so it runs on any distribution with glibc 2.34 or newer (Ubuntu 22.04, Debian 12, Fedora 35,
Arch, openSUSE, SteamOS 3 and newer):

    cmake -S . -B build-portable -G Ninja -DCMAKE_BUILD_TYPE=Release -DNXL_PORTABLE=ON \
          -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-portable
    packaging/appimage.sh build-portable <libSDL3.so.0>

The AppImage brings its own SDL3, which must be built for an old glibc with all its audio and display back ends;
Valve's Steam Runtime "sniper" SDK (Debian 11) has everything for that:

    podman run --rm -v "$PWD:/w" -w /w registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest sh -c \
      'git clone -b release-3.4.0 --depth 1 https://github.com/libsdl-org/SDL && \
       cmake -S SDL -B SDL/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSDL_STATIC=OFF && cmake --build SDL/build && \
       strip SDL/build/libSDL3.so.0'

`packaging/appimage.sh` checks the programs need nothing newer than glibc 2.34, adds SDL3, a font for systems without
one NativeNX knows, the licences, and makes `NativeNX-x86_64.AppImage` with the AppImage project's tools (downloaded
from GitHub when not given). `NativeNX-x86_64.AppImage --nxlinux <game>` runs nxlinux itself.
