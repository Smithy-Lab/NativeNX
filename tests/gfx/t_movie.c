// SPDX-License-Identifier: GPL-3.0-or-later
// The movie decoders (compat/nxnative/movie_codec.c) on small clips made with FFmpeg (tests/data): 10 grey pictures of
// 64x48, the k-th in presentation order of luma 20k + 20, as H.264 High profile with B-frames (one access unit per
// packet, decode order, as the SDK's demuxer hands them over) and as VP9; and 0.2 s of a 1 kHz tone as AAC.
// With no FFmpeg on the machine each decoder must still give black pictures and silence, one per packet.
#include "nxl_movie_codec.h"
#include "hle_movie_avc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail;
#define CHECK(name, cond) do { int ok_ = !!(cond); if (!ok_) g_fail++; printf("%s - %s\n", ok_ ? "ok" : "not ok", name); } while (0)
static uint8_t *slurp(const char *dir, const char *name, size_t *n) {
    char p[1024];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "rb");
    if (!f) { printf("not ok - cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (fread(b, 1, *n, f) != *n) exit(1);
    fclose(f);
    return b;
}
static int luma(const uint8_t *nv12) { return nv12[24 * 64 + 32]; }
// the k-th picture's luma: grey 20k + 20 drawn in full range, encoded in video range (16-235)
static int want(int k) { return (int)(16 + (20 * k + 20) * 219.0 / 255 + 0.5); }

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "tests/data";
    const char *desc = mc_describe();
    int have = strncmp(desc, "no FFmpeg", 9) != 0;
    printf("# %s\n", desc);
    enum { W = 64, H = 48 };
    uint8_t pic[W * H * 4];

    // H.264: split at access unit delimiters (NAL type 9)
    size_t n;
    uint8_t *s = slurp(dir, "movie_h264.264", &n);
    size_t au[64]; int nau = 0;
    for (size_t i = 0; i + 4 < n && nau < 64; i++)
        if (!s[i] && !s[i + 1] && s[i + 2] == 1 && (s[i + 3] & 31) == 9) au[nau++] = i && !s[i - 1] ? i - 1 : i, i += 3;
    au[nau] = n;
    mavc *a = mavc_new();
    int got = 0, lum[16], prev_pts = -1, order_ok = 1;
    for (int k = 0; k < nau; k++) {
        mavc_decode(a, s + au[k], (int)(au[k + 1] - au[k]), (int64_t)k * 100000);
        int64_t t;
        while (got < 16 && mavc_take(a, 0, pic, W, H, &t)) { lum[got++] = luma(pic); if (t < prev_pts) order_ok = 0; prev_pts = (int)t; }
    }
    int64_t t;
    while (got < 16 && mavc_take(a, 1, pic, W, H, &t)) { lum[got++] = luma(pic); if (t < prev_pts) order_ok = 0; prev_pts = (int)t; }
    printf("# H.264: %d access units, %d pictures:", nau, got);
    for (int i = 0; i < got; i++) printf(" %d", lum[i]);
    printf("\n");
    CHECK("H.264: one picture per access unit, the queue empty after the end", got == nau && nau == 10 && mavc_queued(a) == 0);
    int seq = 1;
    for (int i = 0; i < got; i++) seq &= have ? abs(lum[i] - want(i)) <= 3 : lum[i] == 16;
    CHECK(have ? "H.264: pictures in presentation order, their grey as drawn" : "H.264 without FFmpeg: black pictures", seq);
    CHECK("H.264: times come out ascending", order_ok);
    // a flush (seek) and the stream again: the decoder starts over
    mavc_drop(a);
    got = 0;
    for (int k = 0; k < nau; k++) { mavc_decode(a, s + au[k], (int)(au[k + 1] - au[k]), (int64_t)k * 100000); while (got < 16 && mavc_take(a, 0, pic, W, H, &t)) lum[got++] = luma(pic); }
    while (got < 16 && mavc_take(a, 1, pic, W, H, &t)) lum[got++] = luma(pic);
    CHECK("H.264: after a flush the stream decodes again from its start", got == 10 && (!have || abs(lum[0] - want(0)) <= 3));
    mavc_set_rgba(a);
    mavc_drop(a);
    mavc_decode(a, s + au[0], (int)(au[1] - au[0]), 0);
    int rgba_ok = mavc_take(a, 1, pic, W, H, &t) && pic[3] == 255 && (!have || abs(pic[(24 * W + 32) * 4] - 20) <= 4);
    CHECK("H.264: RGBA output (the native texture mode)", rgba_ok);
    mavc_free(a);
    free(s);

    // VP9 in IVF: a 32-byte file header, then per frame 4 bytes of size, 8 of time, the frame
    s = slurp(dir, "movie_vp9.ivf", &n);
    mc_vpx *v = mc_vpx_new(3);
    got = 0;
    int pk = 0, vseq = 1;
    for (size_t i = 32; i + 12 <= n; pk++) {
        uint32_t sz = s[i] | s[i + 1] << 8 | s[i + 2] << 16 | (uint32_t)s[i + 3] << 24;
        if (mc_vpx_decode(v, s + i + 12, (int)sz, pic, W, H, 0)) { int l = luma(pic); vseq &= have ? abs(l - want(got)) <= 6 : l == 16; got++; }
        i += 12 + sz;
    }
    printf("# VP9: %d packets, %d pictures\n", pk, got);
    CHECK(have ? "VP9: a picture per shown frame, their grey as drawn" : "VP9 without FFmpeg: black pictures", got == 10 && vseq);
    mc_vpx_free(v);
    free(s);

    // AAC: ADTS frames; the decoder takes the raw frames and the AudioSpecificConfig made from the first header
    s = slurp(dir, "movie_aac.adts", &n);
    uint8_t asc[2];
    int prof = (s[2] >> 6) + 1, fi = s[2] >> 2 & 15, cc = (s[2] & 1) << 2 | s[3] >> 6;
    asc[0] = (uint8_t)(prof << 3 | fi >> 1), asc[1] = (uint8_t)((fi & 1) << 7 | cc << 3);
    mc_aac *ac = mc_aac_new(asc, 2, 0, 0);
    static int16_t pcm[1 << 16];
    size_t total = 0;
    int frames = 0;
    for (size_t i = 0; i + 7 <= n; frames++) {
        int len = (s[i + 3] & 3) << 11 | s[i + 4] << 3 | s[i + 5] >> 5, hl = s[i + 1] & 1 ? 7 : 9;
        total += mc_aac_decode(ac, s + i + hl, len - hl, pcm + total / 2, sizeof pcm - total);
        i += (size_t)len;
    }
    double e = 0;
    size_t ns = total / 4;
    for (size_t i = ns / 2; i < ns; i++) e += (double)pcm[2 * i] * pcm[2 * i];
    double rms = ns ? sqrt(e / (double)(ns - ns / 2)) : 0;
    printf("# AAC: %d frames, %zu bytes of PCM, rms %.0f\n", frames, total, rms);
    CHECK(have ? "AAC: about 1024 stereo samples per frame, and the tone is heard" : "AAC without FFmpeg: a frame of silence each",
          frames > 5 && total >= (size_t)(frames - 2) * 4096 && total <= (size_t)(frames + 1) * 4096 && (have ? rms > 1000 : rms == 0));
    mc_aac_free(ac);
    free(s);
    printf("nxl-tests: %s\n", g_fail ? "failed" : "passed");
    return g_fail != 0;
}
