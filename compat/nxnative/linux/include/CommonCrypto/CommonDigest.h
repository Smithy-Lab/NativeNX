// SPDX-License-Identifier: GPL-3.0-or-later
// CommonCrypto's one-shot SHA-256 (FIPS 180-4), for nxnative's sources on Linux
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#define CC_SHA256_DIGEST_LENGTH 32
typedef uint32_t CC_LONG;
static inline void nxl_sha256_block(uint32_t h[8], const uint8_t *p) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
        0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
        0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
        0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
#define NXL_ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 64; i++)
        w[i] = w[i - 16] + (NXL_ROR(w[i - 15], 7) ^ NXL_ROR(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
               (NXL_ROR(w[i - 2], 17) ^ NXL_ROR(w[i - 2], 19) ^ (w[i - 2] >> 10));
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = hh + (NXL_ROR(e, 6) ^ NXL_ROR(e, 11) ^ NXL_ROR(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (NXL_ROR(a, 2) ^ NXL_ROR(a, 13) ^ NXL_ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += hh;
#undef NXL_ROR
}
// the streaming form
typedef struct { uint32_t h[8]; uint8_t buf[64]; uint64_t total; size_t n; } CC_SHA256_CTX;
static inline int CC_SHA256_Init(CC_SHA256_CTX *c) {
    static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(c->h, iv, sizeof iv);
    c->total = 0, c->n = 0;
    return 1;
}
static inline int CC_SHA256_Update(CC_SHA256_CTX *c, const void *data, CC_LONG len) {
    const uint8_t *p = data;
    c->total += len;
    while (len) {
        size_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k);
        c->n += k, p += k, len -= (CC_LONG)k;
        if (c->n == 64) nxl_sha256_block(c->h, c->buf), c->n = 0;
    }
    return 1;
}
static inline int CC_SHA256_Final(unsigned char *md, CC_SHA256_CTX *c) {
    uint64_t bits = c->total * 8;
    uint8_t pad = 0x80, zero = 0;
    CC_SHA256_Update(c, &pad, 1);
    while (c->n != 56) CC_SHA256_Update(c, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    CC_SHA256_Update(c, len, 8);
    for (int i = 0; i < 8; i++) md[4 * i] = c->h[i] >> 24, md[4 * i + 1] = c->h[i] >> 16, md[4 * i + 2] = c->h[i] >> 8, md[4 * i + 3] = c->h[i];
    return 1;
}
// the one-shot form
static inline unsigned char *CC_SHA256(const void *data, CC_LONG len, unsigned char *md) {
    CC_SHA256_CTX c;
    CC_SHA256_Init(&c);
    CC_SHA256_Update(&c, data, len);
    CC_SHA256_Final(md, &c);
    return md;
}
