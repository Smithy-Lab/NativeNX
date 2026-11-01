# Notices

nxlinux is licensed under the GNU General Public License, version 3 or later (LICENSE). It links and builds code from
the following projects under their own licenses.

## ScarletNX

The ARM64-to-x86_64 recompiler library (`libscarletnx-recompiler`), fetched at the commit `NXL_SCARLETNX_TAG` in
CMakeLists.txt. GPL-3.0. https://github.com/scarletnx/ScarletNX

## nxnative

Host service sources from `dm/host` (listed in `NXL_NXNATIVE_HLE` in CMakeLists.txt), its `dm.h`, and its game profile
tool `dm/games/game.py`, fetched at the commit `NXL_NXNATIVE_TAG`. nxlinux's boot sequence (`src/main.c`), loader
binding order (`src/loader.c`), libc pass-through list (`src/hle_libc.c`), HLE declaration macros and service event
binding (`compat/nxnative/compat.c`) follow nxnative's code. https://github.com/seedds/nxnative

    MIT License

    Copyright (c) 2026 seedds

    Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
    documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
    Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
    WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
    COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
    OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

## LZ4

`third_party/lz4` (lz4.c, lz4.h), BSD-2-Clause, copyright Yann Collet. See third_party/lz4/LICENSE.

## Khronos registries

`gfx/api/*.sigs` (the OpenGL, EGL and Vulkan functions' signatures) are derived by `tools/gen_gfxapi.py` from Khronos'
API registries (gl.xml, egl.xml: Apache-2.0; vk.xml: Apache-2.0 OR MIT), copyright The Khronos Group Inc.

## References

Horizon's behaviour as implemented here follows Atmosphère's mesosphère, Switchbrew and libnx.

## yuzu's shader recompiler and ASTC decoder (citron fork)

`third_party/shader_recompiler` (GPL-2.0-or-later, used under GPL-3.0) and `third_party/astc` (FasTC, Apache-2.0, with
yuzu's GPL-2.0-or-later changes): see their LICENSE.md files. https://github.com/citron-neo/emulator

## unordered_dense

`third_party/shader_recompiler/ankerl`: MIT License, Copyright (c) 2022 Martin Leitner-Ankerl (its LICENSE file).

## Built with, not included

sirit (MIT, https://github.com/ReinUsesLisp/sirit) and SPIRV-Headers (MIT-style, Khronos), fmt (MIT), Boost (Boost
Software License 1.0), Vulkan-Headers and the Vulkan loader (Apache-2.0), SDL3 (zlib license): from the system or fetched
at configure time.

## stb

`third_party/stb`: `stb_truetype.h`, `stb_image.h` and `stb_image_write.h` by Sean Barrett, unmodified, used by the
launcher for text and pictures. Public domain or MIT (`third_party/stb/LICENSE`). https://github.com/nothings/stb

## Kenney

`launcher/data`: interface sounds from Kenney's *Interface Sounds* and button and controller pictures from Kenney's
*Input Prompts*, unmodified (sounds renamed). Creative Commons Zero (public domain); see `launcher/data/README.md` and
the `LICENSE.txt` in each folder. https://kenney.nl

## Zacksly: Switch Button Icons and Controls

`launcher/data/prompts/zacksly_pro_controller.png`: the Pro Controller icon ("Buttons Full Solid", white, 256 px) from
*Switch Button Icons and Controls* by Zacksly, modified (cropped to the drawing). Licensed under CC BY 3.0
(`launcher/data/prompts/LICENSE-zacksly.txt`, https://creativecommons.org/licenses/by/3.0/). https://zacksly.itch.io

## AMD FidelityFX Super Resolution 1.0 (third_party/fsr)

`ffx_a.h` and `ffx_fsr1.h` from https://github.com/GPUOpen-Effects/FidelityFX-FSR (commit a21ffb8f6c13233ba336352bdff293894c706575),
Copyright (c) 2021 Advanced Micro Devices, Inc., MIT licence (third_party/fsr/LICENSE.txt). Used by gfx/vk_upscale.c and
gfx/shaders/fsr_*.comp to scale the game's frame to the window.

## In the AppImage and the Windows build

The AppImage (packaging/appimage.sh) and the Windows zip also carry, unmodified:

- **SDL3** (zlib license, https://libsdl.org): `libSDL3.so.0` / `SDL3.dll`, built from SDL's own source.
- **DejaVu Sans** fonts (the Bitstream Vera and DejaVu licences, free to redistribute; `LICENSE-DejaVu` beside them), for
  NativeNX's text on systems without a font it knows.
- In the Windows build, statically linked: LLVM's C++ runtime and unwinder (libc++, libunwind: Apache-2.0 with LLVM
  exceptions) and MinGW-w64's runtime and winpthreads (ZPL, MIT and BSD-style licences), from llvm-mingw.
- The AppImage's **runtime** (the AppImage project, MIT, https://github.com/AppImage/type2-runtime).
