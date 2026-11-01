// SPDX-License-Identifier: GPL-3.0-or-later
// The bounds of the tables the linker builds from sections (NXL_SECTION, hle_abi.h), on Windows: the PE linker sorts a
// section's parts by what follows '$', so these empty arrays in "$a" and "$z" come before and after every entry ("$m"),
// as the ELF linker's __start_ and __stop_ symbols do on Linux.
#include "hle_abi.h"

#define BOUNDS(name, type) \
    __attribute__((used, section(#name "$a"), aligned(8))) const type __start_##name[0]; \
    __attribute__((used, section(#name "$z"), aligned(8))) const type __stop_##name[0];
BOUNDS(nxl_hle, hle_ent)
typedef struct { const char *name; void *fn; } nvn_impl;   // (as compat/nxnative/patches/hle_nvn.py declares it)
BOUNDS(nxl_nvn, nvn_impl)
