// SPDX-License-Identifier: GPL-3.0-or-later
// Host implementations of guest functions, declared as in nxnative (dm/host/hle.h):
//   HLE("mangled name", return type, c_name, parameters...)              used when no module defines the name
//   HLE_FORCE("mangled name", "why", return type, c_name, parameters...)  wins over a module's own definition
// On the Mac the game's ARM64 code calls these directly (same ABI). Here the game runs as recompiled code with its
// registers in a CPU struct, so tools/gen_thunks.py reads these declarations and writes, for each, a thunk that takes
// the arguments from the guest registers by the AAPCS64 rules (hle_abi.h), calls the function and writes the result
// back. The build compiles each HLE source together with its generated thunks.
#pragma once
#include "nxl.h"
#include "hle_abi.h"

#ifdef NXL_GEN_SCAN   // tools/gen_thunks.py preprocesses with this: each declaration leaves a marker it reads
#define HLE(mangled, ret, cname, ...) __nxl_hle(0, mangled, NULL, ret, cname, __VA_ARGS__) static ret cname(__VA_ARGS__)
#define HLE_FORCE(mangled, why, ret, cname, ...) __nxl_hle(1, mangled, why, ret, cname, __VA_ARGS__) static ret cname(__VA_ARGS__)
#else
#define HLE(mangled, ret, cname, ...) static ret cname(__VA_ARGS__)
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#endif
