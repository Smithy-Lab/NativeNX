// SPDX-License-Identifier: GPL-3.0-or-later
// A mutational fuzzer for the parsers of files NativeNX reads from outside: SARC, Yaz0, AAMP, BotW's resource size
// table, and whole BCML mods (one of their files damaged, then merged).
// Each run takes a seed file, damages it (bits flipped, bytes set, cut short, stretches copied ...), and runs it through
// the parser; built with AddressSanitizer and UndefinedBehaviorSanitizer a crash or overflow stops it with a report,
// and the input that did it is in <run folder>.cur. Not part of the tests (it takes minutes); docs/DEVELOPING.md says
// how to run it.
// usage: fuzz_parsers <aamp|sarc|yaz0|rstb|bnp> <runs> <seed files...>
//   bnp needs FZ_MOD (a BCML mod folder), FZ_GAMEDIR and FZ_DLCDIR (the game's and the DLC's files); FZ_SEED picks the
//   random sequence
#define _GNU_SOURCE
#include "../../launcher/bnp.h"
#include "../../launcher/aamp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
static uint64_t rs = 88172645463325252ull;
static uint32_t R(void) { rs ^= rs << 13; rs ^= rs >> 7; rs ^= rs << 17; return (uint32_t)rs; }
static uint8_t *slurp(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); if (!f) return NULL; fseek(f, 0, 2); long s = ftell(f); fseek(f, 0, 0); uint8_t *b = malloc(s + 1); *n = fread(b, 1, s, f); fclose(f); return b; }
static void spit(const char *p, const uint8_t *d, size_t n) { FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); }
static uint8_t *mutate(const uint8_t *s, size_t n, size_t *on) {
    size_t cap = n + 4096; uint8_t *o = malloc(cap); memcpy(o, s, n); size_t m = n;
    int k = 1 + R() % 8;
    for (int i = 0; i < k && m; i++) {
        size_t at = R() % m;
        switch (R() % 7) {
        case 0: o[at] ^= 1 << (R() % 8); break;
        case 1: o[at] = (uint8_t)R(); break;
        case 2: { static const uint32_t v[] = {0, 1, 0xFF, 0xFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x10000}; if (at + 4 <= m) memcpy(o + at, &v[R() % 8], 4); break; }
        case 3: m = at + 1; break;   // truncate
        case 4: { size_t len = R() % 64, from = R() % m; if (from + len <= m && at + len <= m) memmove(o + at, o + from, len); break; }
        case 5: if (m + 16 < cap) { memmove(o + at + 8, o + at, m - at); for (int j = 0; j < 8; j++) o[at + j] = (uint8_t)R(); m += 8; } break;
        case 6: if (at + 2 <= m) { o[at] = 0xFF; o[at + 1] = 0xFF; } break;
        }
    }
    *on = m; return o;
}
static void t_aamp(const uint8_t *d, size_t n) {
    aamp_pio p; if (aamp_read(d, n, &p)) return;
    size_t wn; uint8_t *w = aamp_write(&p, &wn);
    aamp_pio q; if (w && !aamp_read(w, wn, &q)) { aamp_merge(&p.root, &q.root, 1); aamp_merge(&q.root, &p.root, 0); size_t x; free(aamp_write(&q, &x)); aamp_free(&q); }
    free(w); aamp_free(&p);
}
static void t_sarc(const uint8_t *d, size_t n) {
    sarc_ent *e; int k = sarc_read(d, n, &e); if (k < 0) return;
    size_t on; free(sarc_write(e, k, &on)); sarc_free(e, k);
    uint8_t *dec; size_t dn; if (!yaz0_decode(d, n, &dec, &dn)) free(dec);
}
static void t_yaz0(const uint8_t *d, size_t n) { uint8_t *o; size_t on; if (!yaz0_decode(d, n, &o, &on)) { size_t sn; free(yaz0_store(o, on, d, &sn)); free(o); } }
static void t_rstb(const uint8_t *d, size_t n) {
    uint8_t *t = malloc(n + 1); memcpy(t, d, n); size_t tn = n; uint32_t v;
    rstb_get(t, tn, "Actor/Pack/A.bactorpack", &v); rstb_set(&t, &tn, "Actor/Pack/A.bactorpack", 5); rstb_set(&t, &tn, "X", 0); rstb_set(&t, &tn, "Y/Z.bfres", 99); rstb_get(t, tn, "Y/Z.bfres", &v);
    free(t);
}
// a whole BCML mod: one of its files damaged, then detected and merged (FZ_MOD: the mod, FZ_GAMEDIR: the game's files)
static int rd_game(void *ctx, const char *path, uint8_t **d, size_t *n) { char p[1200]; snprintf(p, sizeof p, "%s/%s", (const char *)ctx, path); *d = slurp(p, n); return *d ? 0 : -1; }
static const char *const MODF[] = {"info.json", "logs/packs.json", "logs/rstb.json", "logs/deepmerge.aamp", "logs/aslist.aamp",
    "01007EF00011E000/romfs/Actor/Pack/Armor_001_Head.sbactorpack", "01007EF00011E000/romfs/Pack/TitleBG.pack", "options/Physics Opt/logs/packs.json",
    "01007EF00011F000/romfs/Actor/Pack/Aoc_Armor.sbactorpack", "options/Physics Opt/01007EF00011E000/romfs/Actor/Pack/Armor_001_Head.sbactorpack"};
static void t_bnp(const char *tmp) {
    char cmd[1500]; snprintf(cmd, sizeof cmd, "rm -rf '%s/mod' '%s/out' && cp -r '%s' '%s/mod'", tmp, tmp, getenv("FZ_MOD"), tmp); if (system(cmd)) return;
    int k = R() % (sizeof MODF / sizeof *MODF); char p[1200]; snprintf(p, sizeof p, "%s/mod/%s", tmp, MODF[k]);
    size_t n; uint8_t *d = slurp(p, &n); if (!d) return; size_t mn; uint8_t *m = mutate(d, n, &mn); spit(p, m, mn); free(d), free(m);
    char md[700]; snprintf(md, sizeof md, "%s/mod", tmp); bnp_t b; if (!bnp_detect(md, &b)) return;
    uint32_t on = R(); char out[700], err[300]; snprintf(out, sizeof out, "%s/out", tmp);
    bnp_build(&b, &on, 1, rd_game, getenv("FZ_GAMEDIR"), rd_game, getenv("FZ_DLCDIR"), out, err, sizeof err);
}
int main(int argc, char **argv) {
    // fz <mode> <iterations> <seed files...>
    const char *mode = argv[1]; long it = atol(argv[2]);
    if (getenv("FZ_SEED")) rs = strtoull(getenv("FZ_SEED"), 0, 10);
    int ns = argc - 3; uint8_t **s = calloc(ns, sizeof *s); size_t *sn = calloc(ns, sizeof *sn);
    for (int i = 0; i < ns; i++) s[i] = slurp(argv[3 + i], &sn[i]);
    if (argc < 4) { fprintf(stderr, "usage: fuzz_parsers <aamp|sarc|yaz0|rstb|bnp> <runs> <seed files...>\n"); return 2; }
    char tmp[] = "/tmp/fuzz_parsersXXXXXX";
    if (!mkdtemp(tmp)) return 1;
    char cur[600]; snprintf(cur, sizeof cur, "%s.cur", tmp);
    for (long i = 0; i < it; i++) {
        int k = R() % ns; size_t mn; uint8_t *m = i == 0 ? (mn = sn[k], memcpy(malloc(sn[k] + 1), s[k], sn[k])) : mutate(s[k], sn[k], &mn);
        spit(cur, m, mn);
        if (!strcmp(mode, "aamp")) t_aamp(m, mn);
        else if (!strcmp(mode, "sarc")) t_sarc(m, mn);
        else if (!strcmp(mode, "yaz0")) t_yaz0(m, mn);
        else if (!strcmp(mode, "rstb")) t_rstb(m, mn);
        else if (!strcmp(mode, "bnp")) t_bnp(tmp);
        free(m);
    }
    printf("%s: %ld runs, no crash\n", mode, it);
    return 0;
}
