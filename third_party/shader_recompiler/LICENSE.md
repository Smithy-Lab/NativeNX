# yuzu's shader recompiler

`shader_recompiler/` is yuzu's Maxwell shader recompiler as kept in the citron fork
(https://github.com/citron-neo/emulator, `src/shader_recompiler`), SPDX-License-Identifier GPL-2.0-or-later
(Copyright yuzu Emulator Project and citron Emulator Project; see each file's header). nxlinux uses it under the GPL,
version 3 (see the repository's LICENSE). Changes for nxlinux:

- the GLSL and GLASM backends and yuzu's CMake file are not included;
- `backend/spirv/emit_spirv_warp.cpp`: a quad swap is emitted as a lane-XOR shuffle (upstream sirit has no
  `OpGroupNonUniformQuadSwap`).

`common/` holds the few yuzu headers it includes (`common_types.h`, `bit_field.h`, `alignment.h`, `div_ceil.h`,
`common_funcs.h`, `assert.h`, `swap.h`, same license), with `logging.h` and `settings.h` replaced by small nxlinux
versions.

`ankerl/` is Martin Leitner-Ankerl's unordered_dense (MIT License, `ankerl/LICENSE`).
