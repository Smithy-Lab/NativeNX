// SPDX-License-Identifier: GPL-3.0-or-later
// The recompiled code computes what an ARM64 CPU would: known answers, ARM-specific semantics that differ from x86,
// the Cortex-A57's CRC32 instructions, NEON, atomics and deep recursion.
#include "nx.h"
#include <arm_neon.h>
#include <arm_acle.h>

// SHA-256 (FIPS 180-4), checked against the standard's "abc" vector
static const u32 K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha256_abc(u32 out[8]) {
    u8 blk[64] = {'a', 'b', 'c', 0x80};
    blk[63] = 24;   // length in bits
    u32 h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}, w[64];
    for (int i = 0; i < 16; i++) w[i] = (u32)blk[4 * i] << 24 | blk[4 * i + 1] << 16 | blk[4 * i + 2] << 8 | blk[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        u32 s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    u32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        u32 t1 = hh + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        u32 t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    u32 v[8] = {a, b, c, d, e, f, g, hh};
    for (int i = 0; i < 8; i++) out[i] = h[i] + v[i];
}

static u32 crc32_table(const u8 *p, int n) {   // the reflected 0xEDB88320 polynomial, bit by bit
    u32 c = 0xFFFFFFFF;
    for (int i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320 & -(c & 1)); }
    return ~c;
}

static long fib(long n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static long depth(long n) { volatile long pad[8] = {n}; return n ? depth(n - 1) + pad[0] - n + 1 : 0; }

static int jump(int k) {   // a jump table
    switch (k) { case 0: return 11; case 1: return 22; case 2: return 33; case 3: return 44; case 4: return 55; case 5: return 66; default: return -1; }
}

int test_main(void) {
    u32 d[8];
    sha256_abc(d);
    CHECK("SHA-256(\"abc\")", d[0] == 0xba7816bf && d[1] == 0x8f01cfea && d[2] == 0x414140de && d[3] == 0x5dae2223 &&
                               d[4] == 0xb00361a3 && d[5] == 0x96177a9c && d[6] == 0xb410ff61 && d[7] == 0xf20015ad);

    const char *msg = "The quick brown fox jumps over the lazy dog";
    u32 c = 0xFFFFFFFF;
    for (int i = 0; msg[i]; i++) c = __crc32b(c, (u8)msg[i]);
    CHECK("CRC32B instruction (Cortex-A57 CRC extension)", ~c == 0x414FA339 && crc32_table((const u8 *)msg, 43) == 0x414FA339);

    // ARM's float to integer conversion saturates where x86's gives 0x80000000
    volatile double big = 1e20, nan = __builtin_nan(""), neg = -1e20;
    CHECK("double to int saturates high", (int)big == 0x7fffffff);
    CHECK("double to int saturates low", (int)neg == (int)0x80000000);
    CHECK("NaN to int is 0", (int)nan == 0);
    volatile double huge = 1e30;
    CHECK("double to unsigned 64-bit saturates", (u64)huge == 0xffffffffffffffffull);
    volatile s64 mn = (s64)0x8000000000000000ull, mone = -1;
    CHECK("INT64_MIN / -1 is INT64_MIN (no trap on ARM)", mn / mone == mn);
    volatile s64 zero = 0;
    CHECK("division by zero is 0 (no trap on ARM)", (s64)1234 / zero == 0);

    float32x4_t a = {1, 2, 3, 4}, b = {0.5f, 0.25f, 2, -1};
    float32x4_t r = vmlaq_f32(a, a, b);   // a + a*b
    CHECK("NEON multiply-accumulate", vgetq_lane_f32(r, 0) == 1.5f && vgetq_lane_f32(r, 1) == 2.5f && vgetq_lane_f32(r, 2) == 9 && vgetq_lane_f32(r, 3) == 0);
    uint8x16_t bytes = vdupq_n_u8(7);
    CHECK("NEON horizontal add", vaddvq_u8(bytes) == 112);
    int32x4_t sat = vqaddq_s32(vdupq_n_s32(0x7ffffff0), vdupq_n_s32(0x100));
    CHECK("NEON saturating add", vgetq_lane_s32(sat, 2) == 0x7fffffff);

    unsigned __int128 p = (unsigned __int128)0xFFFFFFFFFFFFFFFFull * 0xFFFFFFFFFFFFFFFFull;
    CHECK("128-bit multiply (UMULH)", (u64)(p >> 64) == 0xFFFFFFFFFFFFFFFEull && (u64)p == 1);
    CHECK("count leading zeros, reverse bits", __builtin_clzl(1) == 63 && __builtin_arm_rbit(1) == 0x80000000u);

    long ctr = 0;
    for (int i = 0; i < 1000; i++) __atomic_fetch_add(&ctr, 3, __ATOMIC_SEQ_CST);
    long expect = 3000, desired = 1;
    CHECK("atomics: fetch-add, compare-exchange", ctr == 3000 && __atomic_compare_exchange_n(&ctr, &expect, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) && ctr == 1);

    CHECK("recursion: fib(24)", fib(24) == 46368);
    CHECK("deep recursion (50000 frames)", depth(50000) == 50000);
    int jt = 0;
    for (int k = -1; k < 7; k++) jt += jump(k);
    CHECK("jump table", jt == 11 + 22 + 33 + 44 + 55 + 66 - 2);
    return 0;
}
