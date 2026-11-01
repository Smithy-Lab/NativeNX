// SPDX-License-Identifier: GPL-3.0-or-later
// Moving a word of nxnative's frame step stubs (tools/gen_fstep.py) from where the generator assembled it to where the loader
// puts it (loader.c install_fstep). The generator assembles the stubs at a placeholder P0 and their state page at S0, both above
// every image offset they reach; here a B / BL or an ADRP is pointed again at its target's new place: a placeholder offset in
// the stubs → p + the same distance, in the state page → st + it, an image offset as it was. Other words are copied: the stubs'
// own conditional branches stay inside them and move with them. Offsets are from the module's base (4 KiB aligned)
#include <stdint.h>
#define FS_STUBS_MAX 0x100000ull
#define FS_STATE_SIZE 0x4000ull
static int64_t sx(uint64_t v, int bits) { return (int64_t)(v << (64 - bits)) >> (64 - bits); }
int nxl_fstep_reloc(uint64_t p0, uint64_t s0, uint64_t p, uint64_t st, uint64_t at0, uint64_t at, uint32_t w, uint32_t *out, uint64_t *branch_to);
// at0: the word's offset when assembled, at: where it goes. branch_to: a B / BL's new target (0 for other words). 0: out of reach
int nxl_fstep_reloc(uint64_t p0, uint64_t s0, uint64_t p, uint64_t st, uint64_t at0, uint64_t at, uint32_t w, uint32_t *out, uint64_t *branch_to) {
    uint64_t t;
    int adrp = (w & 0x9F000000u) == 0x90000000u;
    *branch_to = 0;
    if ((w & 0x7C000000u) == 0x14000000u) t = at0 + 4 * (uint64_t)sx(w & 0x3FFFFFF, 26);
    else if (adrp) t = (at0 & ~0xFFFull) + ((uint64_t)sx(((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2), 21) << 12);
    else { *out = w; return 1; }
    if (t - p0 < FS_STUBS_MAX) t = t - p0 + p;
    else if (t - s0 < FS_STATE_SIZE) t = t - s0 + st;
    if (adrp) {
        int64_t imm = ((int64_t)(t & ~0xFFFull) - (int64_t)(at & ~0xFFFull)) >> 12;
        if (imm < -(1 << 20) || imm >= (1 << 20)) return 0;
        *out = (w & 0x9F00001Fu) | (uint32_t)(imm & 3) << 29 | (uint32_t)((imm >> 2) & 0x7FFFF) << 5;
    } else {
        int64_t d = (int64_t)t - (int64_t)at;
        if (d < -(1 << 27) || d >= (1 << 27)) return 0;
        *out = (w & 0xFC000000u) | ((uint32_t)(d >> 2) & 0x3FFFFFF);
        *branch_to = t;
    }
    return 1;
}
