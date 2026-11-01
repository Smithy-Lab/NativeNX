// SPDX-License-Identifier: GPL-3.0-or-later
// tests/maxwell_asm.h: a few Maxwell instructions, encoded by hand (the opcode patterns of yuzu's maxwell.inc), and NVN code areas built
// from them, for the shader and rendering tests: a vertex shader that passes attribute 0 to the position and
// attribute 1 to generic output 0, and a fragment shader that writes generic input 0 (or a constant) to target 0.
#pragma once
#include <stdint.h>
// builtins: this header is also compiled for the guest, which has no C library headers
#define memcpy __builtin_memcpy
#define memset __builtin_memset

#define MX_PT (7ull << 16)   // guard predicate PT
#define MX_RZ 255u
static inline uint64_t mx_ald(unsigned dst, unsigned attr_byte, unsigned n) {   // ALD.n Rdst, a[attr_byte]
    return 0xEFD8000000000000ull | MX_PT | (uint64_t)(n - 1) << 47 | (uint64_t)MX_RZ << 39 | (uint64_t)attr_byte << 20 | (uint64_t)MX_RZ << 8 | dst;
}
static inline uint64_t mx_ast(unsigned src, unsigned attr_byte, unsigned n) {   // AST.n a[attr_byte], Rsrc
    return 0xEFF0000000000000ull | MX_PT | (uint64_t)(n - 1) << 47 | (uint64_t)MX_RZ << 39 | (uint64_t)attr_byte << 20 | (uint64_t)MX_RZ << 8 | src;
}
static inline uint64_t mx_mov32i(unsigned dst, uint32_t imm) { return 0x0100000000000000ull | MX_PT | (uint64_t)imm << 20 | 0xFull << 12 | dst; }
// IPA.PASS Rdst, a[attr_byte] (attribute field in words)
static inline uint64_t mx_ipa(unsigned dst, unsigned attr_byte) {
    return 0xE000000000000000ull | MX_PT | (uint64_t)(attr_byte / 4) << 30 | (uint64_t)MX_RZ << 20 | (uint64_t)MX_RZ << 8 | dst;
}
// TEX.2D Rdst, Rcoord (x, y), the bound texture whose handle is at c2[handle_byte], rgba into Rdst..Rdst+3
static inline uint64_t mx_tex2d(unsigned dst, unsigned coord, unsigned handle_byte) {
    return 0xC000000000000000ull | MX_PT | (uint64_t)(handle_byte / 4) << 36 | 0xFull << 31 | 2ull << 28 | (uint64_t)MX_RZ << 20 | (uint64_t)coord << 8 | dst;
}
// MOV Rdst, c[bank][byte_off]
static inline uint64_t mx_mov_cbuf(unsigned dst, unsigned bank, unsigned byte_off) {
    return 0x4C98000000000000ull | MX_PT | 0xFull << 39 | (uint64_t)bank << 34 | (uint64_t)(byte_off / 4) << 20 | dst;
}
// FMUL32I Rdst, Rsrc, imm (a float)
static inline uint64_t mx_fmul32i(unsigned dst, unsigned src, float imm) {
    uint32_t b; memcpy(&b, &imm, 4);
    return 0x1E00000000000000ull | MX_PT | (uint64_t)b << 20 | (uint64_t)src << 8 | dst;
}
// F2I.S32.F32.FLOOR Rdst, Rsrc
static inline uint64_t mx_f2i(unsigned dst, unsigned src) {
    return 0x5CB0000000000000ull | MX_PT | 1ull << 39 | (uint64_t)src << 20 | 1ull << 12 | 2ull << 10 | 2ull << 8 | dst;
}
// I2F.F32.U32 Rdst, Rsrc
static inline uint64_t mx_i2f(unsigned dst, unsigned src) {
    return 0x5CB8000000000000ull | MX_PT | (uint64_t)src << 20 | 2ull << 10 | 2ull << 8 | dst;
}
// TLD.LZ.2D Rdst, Rcoord (integer x, y), the texture bound at c2[handle_byte]: a texel fetch, rgba into Rdst..Rdst+3
static inline uint64_t mx_tld2d(unsigned dst, unsigned coord, unsigned handle_byte) {
    return 0xDC00000000000000ull | MX_PT | (uint64_t)(handle_byte / 4) << 36 | 0xFull << 31 | 2ull << 28 | (uint64_t)MX_RZ << 20 | (uint64_t)coord << 8 | dst;
}
// TXQ.DIMENSION Rdst, the texture bound at c2[handle_byte]: width, height into Rdst, Rdst+1 (level 0)
static inline uint64_t mx_txq(unsigned dst, unsigned handle_byte) {
    return (0x1BE9ull << 51) | MX_PT | (uint64_t)(handle_byte / 4) << 36 | 3ull << 31 | 1ull << 22 | (uint64_t)MX_RZ << 8 | dst;
}
static inline uint64_t mx_exit(void) { return 0xE300000000000000ull | MX_PT | 0xF; }
static inline uint64_t mx_nop(void) { return 0x50B0000000000000ull | MX_PT; }
#define MX_SCHED 0x001F8000FC0007E0ull

// A code area: 0x30-byte header (graphics magic), the 0x50-byte SPH, then the instructions with a scheduling word
// before every three. Returns the length
static inline uint32_t mx_code_area(uint8_t *out, const uint8_t sph[0x50], const uint64_t *ins, unsigned n) {
    memset(out, 0, 0x80);
    uint32_t magic = 0x12345678;
    memcpy(out, &magic, 4);
    memcpy(out + 0x30, sph, 0x50);
    uint32_t p = 0x80;
    for (unsigned i = 0; i < n; i++) {
        if (i % 3 == 0) { uint64_t s = MX_SCHED; memcpy(out + p, &s, 8); p += 8; }
        memcpy(out + p, &ins[i], 8); p += 8;
    }
    while ((p - 0x80) % 32) { uint64_t z = (p - 0x80) % 32 == 0 ? MX_SCHED : mx_nop(); memcpy(out + p, &z, 8); p += 8; }
    return p;
}
static inline void mx_sph_vs(uint8_t sph[0x50]) {
    memset(sph, 0, 0x50);
    uint32_t c0 = 1 | 3u << 5 | 1u << 10;   // SPH type 1, version 3, vertex shader
    memcpy(sph, &c0, 4);
    sph[0x18] = 0xFF;   // inputs: generics 0 and 1, xyzw
    sph[0x35] = 0xF0;   // outputs: position xyzw
    sph[0x36] = 0x0F;   // outputs: generic 0 xyzw
}
static inline void mx_sph_fs(uint8_t sph[0x50], int reads_generic0) {
    memset(sph, 0, 0x50);
    uint32_t c0 = 1 | 3u << 5 | 5u << 10;   // SPH type 1, version 3, pixel shader
    memcpy(sph, &c0, 4);
    if (reads_generic0) sph[0x18] = 0xAA;   // generic 0 xyzw, perspective
    uint32_t target = 0xF;                  // render target 0 rgba
    memcpy(sph + 0x48, &target, 4);
}
// the vertex shader: position = attribute 0, generic 0 = attribute 1
static inline uint32_t mx_vs_passthrough(uint8_t *out) {
    uint8_t sph[0x50]; mx_sph_vs(sph);
    uint64_t ins[] = {mx_ald(0, 0x80, 4), mx_ald(4, 0x90, 4), mx_ast(0, 0x70, 4), mx_ast(4, 0x80, 4), mx_exit()};
    return mx_code_area(out, sph, ins, 5);
}
// the fragment shader that samples: color 0 = the texture bound to combined slot 0 (c2 0x20) at generic 0's x, y
static inline uint32_t mx_fs_texture(uint8_t *out) {
    uint8_t sph[0x50]; mx_sph_fs(sph, 1);
    uint64_t ins[] = {mx_ipa(4, 0x80), mx_ipa(5, 0x84), mx_tex2d(0, 4, 0x20), mx_exit()};
    return mx_code_area(out, sph, ins, 4);
}
// the fragment shader that fetches texels: color 0 = texel (8 * fragment x, 8 * fragment y) of the texture at c2 0x20
static inline uint32_t mx_fs_fetch(uint8_t *out) {
    uint8_t sph[0x50]; mx_sph_fs(sph, 0);
    uint64_t ins[] = {mx_ipa(4, 0x70), mx_ipa(5, 0x74), mx_fmul32i(4, 4, 8.0f), mx_fmul32i(5, 5, 8.0f), mx_f2i(4, 4), mx_f2i(5, 5),
                      mx_tld2d(0, 4, 0x20), mx_exit()};
    return mx_code_area(out, sph, ins, 8);
}
// the fragment shader that asks a texture's size: color 0 = (width / 512, height / 512, 0, 1) of the texture at c2 0x20
static inline uint32_t mx_fs_size(uint8_t *out) {
    uint8_t sph[0x50]; mx_sph_fs(sph, 0);
    uint64_t ins[] = {mx_txq(0, 0x20), mx_i2f(0, 0), mx_i2f(1, 1), mx_fmul32i(0, 0, 1.0f / 512), mx_fmul32i(1, 1, 1.0f / 512),
                      mx_mov32i(2, 0), mx_mov32i(3, 0x3F800000), mx_exit()};
    return mx_code_area(out, sph, ins, 8);
}
// the fragment shader that reads a uniform buffer: color 0 = the first 16 bytes of uniform buffer 0 (c3)
static inline uint32_t mx_fs_uniform(uint8_t *out) {
    uint8_t sph[0x50]; mx_sph_fs(sph, 0);
    uint64_t ins[] = {mx_mov_cbuf(0, 3, 0), mx_mov_cbuf(1, 3, 4), mx_mov_cbuf(2, 3, 8), mx_mov_cbuf(3, 3, 12), mx_exit()};
    return mx_code_area(out, sph, ins, 5);
}
// the fragment shader: color 0 = generic 0, or the constant rgba when varying is 0
static inline uint32_t mx_fs_color(uint8_t *out, int varying, const float rgba[4]) {
    uint8_t sph[0x50]; mx_sph_fs(sph, varying);
    uint64_t ins[8]; unsigned n = 0;
    for (int i = 0; i < 4; i++) {
        if (varying) ins[n++] = mx_ipa(i, 0x80 + 4 * i);
        else { uint32_t b; memcpy(&b, &rgba[i], 4); ins[n++] = mx_mov32i(i, b); }
    }
    ins[n++] = mx_exit();
    return mx_code_area(out, sph, ins, n);
}
