// SPDX-License-Identifier: GPL-3.0-or-later
// BCML mods for Breath of the Wild (bnp.h). Formats as ZeldaMods documents them: Yaz0, SARC (little-endian on the
// Switch, name hash key 0x65), the resource size table (RSTB: CRC-32 of a name and its size, plus a table of names for
// the ones whose CRC-32 collide).
#define _GNU_SOURCE
#include "bnp.h"
#include "aamp.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(char *err, size_t en, const char *fmt, ...) {
    if (err && en) { va_list ap; va_start(ap, fmt); vsnprintf(err, en, fmt, ap); va_end(ap); }
    return -1;
}
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
// Reads a whole file into memory (NUL-ended); NULL when it cannot.
static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long s = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = s >= 0 ? malloc((size_t)s + 1) : NULL;
    if (b && fread(b, 1, (size_t)s, f) != (size_t)s) { free(b); b = NULL; }
    fclose(f);
    if (b) b[s] = 0, *n = (size_t)s;
    return b;
}
static int mkdirs(const char *p) {
    char b[2048];
    snprintf(b, sizeof b, "%s", p);
    for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0755); *s = '/'; }
    return mkdir(b, 0755) && errno != EEXIST ? -1 : 0;
}
// Writes a file, making its folder first; 0 on success.
static int write_file(const char *p, const void *d, size_t n) {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s", p);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdirs(dir); }
    FILE *f = fopen(p, "wb");
    if (!f) return -1;
    int ok = fwrite(d, 1, n, f) == n;
    return fclose(f) == 0 && ok ? 0 : -1;
}

// ---------- a little JSON (info.json, packs.json, rstb.json) ----------
typedef struct jv { int t; double num; char *s; struct jv *v; char **k; int n; } jv;   // t: 0 null, 1 bool, 2 number, 3 string, 4 array, 5 object
static void jfree(jv *j) {
    for (int i = 0; i < j->n; i++) { jfree(&j->v[i]); if (j->k) free(j->k[i]); }
    free(j->v), free(j->k), free(j->s);
}
static const char *jws(const char *p) { while (*p && isspace((unsigned char)*p)) p++; return p; }
static const char *jstr(const char *p, char **out) {   // p at the opening quote
    size_t cap = 64, n = 0;
    char *s = malloc(cap);
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p[1]) {
            p++;
            c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p == 'b' ? '\b' : *p == 'f' ? '\f' : *p;
            if (*p == 'u' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2]) && isxdigit((unsigned char)p[3]) && isxdigit((unsigned char)p[4])) {
                unsigned u = (unsigned)strtoul((char[]){p[1], p[2], p[3], p[4], 0}, NULL, 16);
                p += 4;
                if (n + 4 >= cap) s = realloc(s, cap *= 2);
                if (u < 0x80) s[n++] = (char)u;
                else if (u < 0x800) s[n++] = (char)(0xC0 | u >> 6), s[n++] = (char)(0x80 | (u & 63));
                else s[n++] = (char)(0xE0 | u >> 12), s[n++] = (char)(0x80 | (u >> 6 & 63)), s[n++] = (char)(0x80 | (u & 63));
                continue;
            }
        }
        if (n + 2 >= cap) s = realloc(s, cap *= 2);
        s[n++] = c;
    }
    s[n] = 0;
    *out = s;
    return *p == '"' ? p + 1 : p;
}
// Parses a JSON value (strings, numbers, booleans, null, arrays, objects up to 32 deep); NULL when it does not read.
static const char *jparse(const char *p, jv *j, int depth) {
    memset(j, 0, sizeof *j);
    p = jws(p);
    if (depth > 32 || !*p) return NULL;
    if (*p == '"') { j->t = 3; return jstr(p, &j->s); }
    if (*p == '{' || *p == '[') {
        int obj = *p == '{';
        j->t = obj ? 5 : 4;
        p = jws(p + 1);
        while (p && *p && *p != (obj ? '}' : ']')) {
            j->v = realloc(j->v, (size_t)(j->n + 1) * sizeof *j->v);
            if (obj) {
                j->k = realloc(j->k, (size_t)(j->n + 1) * sizeof *j->k);
                if (*p != '"') return NULL;
                p = jws(jstr(p, &j->k[j->n]));
                if (*p != ':') { free(j->k[j->n]); return NULL; }
                p++;
            }
            p = jparse(p, &j->v[j->n], depth + 1);
            if (!p) return NULL;
            j->n++;
            p = jws(p);
            if (*p == ',') p = jws(p + 1);
        }
        return p && *p ? p + 1 : NULL;
    }
    if (!strncmp(p, "true", 4)) { j->t = 1, j->num = 1; return p + 4; }
    if (!strncmp(p, "false", 5)) { j->t = 1; return p + 5; }
    if (!strncmp(p, "null", 4)) return p + 4;
    char *e;
    j->num = strtod(p, &e);
    if (e == p) return NULL;
    j->t = 2;
    return e;
}
// Reads a JSON file; 0 on success.
static int jload(const char *path, jv *j) {
    size_t n;
    char *t = (char *)slurp(path, &n);
    if (!t) return -1;
    const char *e = jparse(t, j, 0);
    free(t);
    if (!e) { jfree(j); memset(j, 0, sizeof *j); return -1; }
    return 0;
}
static const jv *jget(const jv *o, const char *key) {
    for (int i = 0; o && o->t == 5 && i < o->n; i++) if (!strcmp(o->k[i], key)) return &o->v[i];
    return NULL;
}

// ---------- detection ----------
static int aoc_name(const char *name) {   // a folder of DLC files: BotW's DLC title ID (01007EF00011F000) or "aoc"
    return strcasestr(name, "11f0") || !strcasecmp(name, "aoc");
}
// <dir>/romfs or <dir>/<program ID>/romfs (aoc 0: the game's files); <dir>/<DLC ID>/romfs (aoc 1: the DLC's)
static int find_romfs_of(const char *dir, int aoc, char *out, size_t n) {
    char p[1300];
    snprintf(p, sizeof p, "%s/romfs", dir);
    if (!aoc && is_dir(p)) { snprintf(out, n, "%s", p); return 1; }
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    while (d && !found && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "logs") || !strcasecmp(e->d_name, "options")) continue;
        if (aoc_name(e->d_name) != aoc) continue;
        snprintf(p, sizeof p, "%s/%s/romfs", dir, e->d_name);
        if (is_dir(p)) { snprintf(out, n, "%s", p); found = 1; }
    }
    if (d) closedir(d);
    return found;
}
static int find_romfs(const char *dir, char *out, size_t n) { return find_romfs_of(dir, 0, out, n); }
// A BCML mod's folder: its info.json read (name, version, options) and its files found (the game's and the DLC's).
int bnp_detect(const char *dir, bnp_t *b) {
    char p[1300];
    memset(b, 0, sizeof *b);
    snprintf(p, sizeof p, "%s/info.json", dir);
    char lg[1300];
    snprintf(lg, sizeof lg, "%s/logs", dir);
    if (!is_file(p) || !is_dir(lg)) return 0;
    snprintf(b->dir, sizeof b->dir, "%s", dir);
    find_romfs(dir, b->romfs, sizeof b->romfs);
    find_romfs_of(dir, 1, b->aoc, sizeof b->aoc);
    jv j;
    if (jload(p, &j)) return 1;   // a BCML mod whose info.json does not read: no name, no options
    const jv *v = jget(&j, "name");
    if (v && v->t == 3) snprintf(b->name, sizeof b->name, "%s", v->s);
    if ((v = jget(&j, "version")) && v->t == 3) snprintf(b->version, sizeof b->version, "%s", v->s);
    const jv *opts = jget(&j, "options");
    const char *kinds[2] = {"multi", "single"};
    for (int k = 0; k < 2; k++) {
        const jv *list = jget(opts, kinds[k]);
        for (int i = 0; list && list->t == 4 && i < list->n && b->nopt < 16; i++) {
            const jv *o = &list->v[i], *f = jget(o, "folder"), *nm = jget(o, "name"), *ds = jget(o, "desc"), *df = jget(o, "default");
            if (!f || f->t != 3) continue;
            bnp_opt_t *t = &b->opt[b->nopt++];
            snprintf(t->folder, sizeof t->folder, "%s", f->s);
            snprintf(t->name, sizeof t->name, "%s", nm && nm->t == 3 ? nm->s : f->s);
            snprintf(t->desc, sizeof t->desc, "%s", ds && ds->t == 3 ? ds->s : "");
            t->def = df && df->t == 1 ? df->num != 0 : 1;
        }
    }
    jfree(&j);
    return 1;
}

// ---------- Yaz0 ----------
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
// Decompresses Yaz0 (groups of 8: a byte as it is, or a copy from up to 4 KiB back); 0 when it read whole.
int yaz0_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *on) {
    if (n < 16 || memcmp(in, "Yaz0", 4)) return -1;
    size_t size = be32(in + 4), i = 16, o = 0;
    uint8_t *d = malloc(size ? size : 1);
    if (!d) return -1;
    while (o < size && i < n) {
        uint8_t flags = in[i++];
        for (int b = 7; b >= 0 && o < size && i < n; b--) {
            if (flags >> b & 1) { d[o++] = in[i++]; continue; }
            if (i + 1 >= n) break;
            uint32_t b1 = in[i], b2 = in[i + 1], dist = ((b1 & 15) << 8 | b2) + 1, len;
            i += 2;
            if (b1 >> 4) len = (b1 >> 4) + 2;
            else { if (i >= n) break; len = (uint32_t)in[i++] + 0x12; }
            if (dist > o) { free(d); return -1; }
            for (uint32_t k = 0; k < len && o < size; k++, o++) d[o] = d[o - dist];
        }
    }
    if (o != size) { free(d); return -1; }
    *out = d, *on = size;
    return 0;
}
// Yaz0 without compression (every byte stored as it is): what the game reads, quicker to make than compressing.
uint8_t *yaz0_store(const uint8_t *in, size_t n, const uint8_t *hdr, size_t *on) {
    uint8_t *o = malloc(16 + n + n / 8 + 2);
    if (!o) return NULL;
    if (hdr) memcpy(o, hdr, 16); else memset(o, 0, 16);
    memcpy(o, "Yaz0", 4);
    o[4] = (uint8_t)(n >> 24), o[5] = (uint8_t)(n >> 16), o[6] = (uint8_t)(n >> 8), o[7] = (uint8_t)n;
    size_t w = 16;
    for (size_t i = 0; i < n; i += 8) {
        o[w++] = 0xFF;   // eight bytes as they are
        size_t k = n - i < 8 ? n - i : 8;
        memcpy(o + w, in + i, k);
        w += k;
    }
    *on = w;
    return o;
}

// ---------- SARC ----------
static uint16_t rd16(const uint8_t *p, int be) { return be ? (uint16_t)(p[0] << 8 | p[1]) : (uint16_t)(p[1] << 8 | p[0]); }
static uint32_t rd32(const uint8_t *p, int be) { return be ? be32(p) : (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static uint32_t sarc_hash(const char *s) { uint32_t h = 0; for (; *s; s++) h = h * 0x65 + (uint32_t)(int32_t)(signed char)*s; return h; }
static uint32_t align_of(size_t off) {   // the largest power of two the offset is a multiple of (4 to 0x2000)
    uint32_t a = 4;
    while (a < 0x2000 && !(off & (a * 2 - 1)) ) a *= 2;
    return off ? a : 0x2000;
}
// A SARC's files (pointing into it; either byte order), their count; -1 when it is not a SARC.
int sarc_read(const uint8_t *in, size_t n, sarc_ent **out) {
    if (n < 0x28 || memcmp(in, "SARC", 4)) return -1;
    int be = in[6] == 0xFE && in[7] == 0xFF;
    uint16_t hs = rd16(in + 4, be);
    uint32_t data = rd32(in + 0xC, be);
    if (hs + 0xC > n || memcmp(in + hs, "SFAT", 4)) return -1;
    uint16_t cnt = rd16(in + hs + 6, be);
    size_t nodes = hs + 0xC, sfnt = nodes + (size_t)cnt * 0x10;
    if (sfnt + 8 > n || memcmp(in + sfnt, "SFNT", 4)) return -1;
    size_t names = sfnt + rd16(in + sfnt + 4, be);
    sarc_ent *e = calloc(cnt ? cnt : 1, sizeof *e);
    for (int i = 0; i < cnt; i++) {
        const uint8_t *nd = in + nodes + 0x10 * i;
        uint32_t attr = rd32(nd + 4, be), s = rd32(nd + 8, be), t = rd32(nd + 12, be);
        if ((size_t)data + t > n || s > t) { sarc_free(e, i); return -1; }
        char nm[32];
        const char *name = nm;
        if (attr >> 24) { size_t no = names + (size_t)(attr & 0xFFFFFF) * 4; name = no < n ? (const char *)in + no : ""; }
        else snprintf(nm, sizeof nm, "%08x", rd32(nd, be));   // (a nameless file)
        e[i].name = strndup(name, n - (size_t)(name - (const char *)in));
        e[i].data = in + data + s, e[i].size = t - s, e[i].align = align_of(data + s);
    }
    *out = e;
    return cnt;
}
void sarc_free(sarc_ent *e, int n) { for (int i = 0; i < n; i++) free(e[i].name), free(e[i].own); free(e); }
static int by_hash(const void *a, const void *b) {
    uint32_t x = sarc_hash(((const sarc_ent *)a)->name), y = sarc_hash(((const sarc_ent *)b)->name);
    return x < y ? -1 : x > y ? 1 : strcmp(((const sarc_ent *)a)->name, ((const sarc_ent *)b)->name);
}
// A little-endian SARC of files, sorted by their name's hash, each at its alignment.
uint8_t *sarc_write(sarc_ent *e, int n, size_t *on) {
    qsort(e, (size_t)n, sizeof *e, by_hash);
    size_t nlen = 0;
    uint32_t maxal = 4;
    for (int i = 0; i < n; i++) { nlen += (strlen(e[i].name) + 4) & ~3u; if (e[i].align > maxal) maxal = e[i].align; }
    size_t head = 0x14 + 0xC + (size_t)n * 0x10 + 8 + nlen, data = (head + maxal - 1) & ~(size_t)(maxal - 1), at = 0;
    for (int i = 0; i < n; i++) { uint32_t a = e[i].align ? e[i].align : 4; at = (at + a - 1) & ~(size_t)(a - 1); at += e[i].size; }
    size_t total = data + at;
    uint8_t *o = calloc(1, total ? total : 1);
    if (!o) return NULL;
    memcpy(o, "SARC", 4), wr16(o + 4, 0x14), o[6] = 0xFF, o[7] = 0xFE, wr32(o + 8, (uint32_t)total), wr32(o + 0xC, (uint32_t)data), wr16(o + 0x10, 0x100);
    uint8_t *sfat = o + 0x14;
    memcpy(sfat, "SFAT", 4), wr16(sfat + 4, 0xC), wr16(sfat + 6, (uint16_t)n), wr32(sfat + 8, 0x65);
    uint8_t *sfnt = sfat + 0xC + n * 0x10, *nm = sfnt + 8;
    memcpy(sfnt, "SFNT", 4), wr16(sfnt + 4, 8);
    size_t no = 0;
    at = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *nd = sfat + 0xC + 0x10 * i;
        uint32_t a = e[i].align ? e[i].align : 4;
        at = (at + a - 1) & ~(size_t)(a - 1);
        wr32(nd, sarc_hash(e[i].name)), wr32(nd + 4, 0x01000000u | (uint32_t)(no / 4)), wr32(nd + 8, (uint32_t)at), wr32(nd + 12, (uint32_t)(at + e[i].size));
        memcpy(o + data + at, e[i].data, e[i].size);
        at += e[i].size;
        strcpy((char *)nm + no, e[i].name);
        no += (strlen(e[i].name) + 4) & ~3u;
    }
    *on = total;
    return o;
}
// The files this merge made or took from a mod, for the size table: BCML gives each its size the way the game's
// resource system needs it (botw rstb's SizeCalculator, and botw-utils' estimates for parameter files and models), as
// its RstbMerger does for what it installs. name: the path as it is (top: a file of the RomFS, its extension "s" and
// all; else a file in a pack); size: decompressed.
typedef struct { char name[512]; size_t size; int top; } made_t;
static made_t *g_made;
static int g_nmade;
static void canon(const char *rel, char *out, size_t n);
static const char *g_prefix = "";   // "Aoc/0010/" while the DLC's files are merged: the size table's names for them
static void made_top(const char *rel, size_t size, int top) {
    g_made = realloc(g_made, (size_t)(g_nmade + 1) * sizeof *g_made);
    snprintf(g_made[g_nmade].name, sizeof g_made[0].name, "%s%s", g_prefix, rel);
    g_made[g_nmade].size = size, g_made[g_nmade++].top = top;
}
static void made(const char *rel, size_t size) { made_top(rel, size, 0); }
static const char *ext_of(const char *n) { const char *d = strrchr(n, '.'), *s = strrchr(n, '/'); return d && (!s || d > s) ? d : ""; }
static int sarc_ext(const char *ext) {   // BCML's SARC_EXTS without its SARC_EXCLUDES
    static const char *const x[] = {".pack", ".bactorpack", ".bmodelsh", ".beventpack", ".stats", ".sbactorpack", ".sbmodelsh", ".sbeventpack", ".sstats"};
    for (size_t i = 0; i < sizeof x / sizeof *x; i++) if (!strcmp(ext, x[i])) return 1;
    return 0;
}
static size_t real_size(const uint8_t *d, size_t n) { return n >= 16 && !memcmp(d, "Yaz0", 4) ? be32(d + 4) : n; }
// a file of a pack the mod brings whole (new, or in place of the game's): it, and what is in it when it is a pack
static void made_whole(const char *name, const uint8_t *d, size_t n, int depth) {
    made(name, real_size(d, n));
    char c[512];
    canon(name, c, sizeof c);
    if (depth > 8 || !sarc_ext(ext_of(c))) return;
    uint8_t *dec = NULL;
    size_t dn;
    if (n >= 16 && !memcmp(d, "Yaz0", 4)) { if (yaz0_decode(d, n, &dec, &dn)) return; d = dec, n = dn; }
    sarc_ent *e;
    int k = sarc_read(d, n, &e);
    for (int i = 0; i < k; i++) made_whole(e[i].name, e[i].data, e[i].size, depth + 1);
    if (k >= 0) sarc_free(e, k);
    free(dec);
}
// a SARC's files with another's laid over them (packs inside packs merged the same way); NULL when either is not one
static uint8_t *sarc_merge(const uint8_t *base, size_t bn, const uint8_t *over, size_t vn, size_t *on, int depth) {
    sarc_ent *b, *v;
    int nb = sarc_read(base, bn, &b);
    if (nb < 0) return NULL;
    int nv = sarc_read(over, vn, &v);
    if (nv < 0) { sarc_free(b, nb); return NULL; }
    sarc_ent *r = calloc((size_t)(nb + nv) + 1, sizeof *r);
    int nr = 0;
    for (int i = 0; i < nb; i++) r[nr] = b[i], r[nr].own = NULL, r[nr++].name = strdup(b[i].name);
    for (int i = 0; i < nv; i++) {
        int k = 0;
        while (k < nr && strcmp(r[k].name, v[i].name)) k++;
        if (k == nr) { made_whole(v[i].name, v[i].data, v[i].size, depth + 1); r[nr] = v[i], r[nr].own = NULL, r[nr++].name = strdup(v[i].name); continue; }
        // in both: a pack inside the pack merged, any other file the mod's
        const uint8_t *bd = r[k].data, *vd = v[i].data;
        size_t bsz = r[k].size, vsz = v[i].size;
        uint8_t *bdec = NULL, *vdec = NULL, *m = NULL;
        size_t bdn = 0, vdn = 0, mn = 0;
        int byaz = bsz >= 16 && !memcmp(bd, "Yaz0", 4), vyaz = vsz >= 16 && !memcmp(vd, "Yaz0", 4);
        if (byaz && !yaz0_decode(bd, bsz, &bdec, &bdn)) bd = bdec, bsz = bdn;
        if (vyaz && !yaz0_decode(vd, vsz, &vdec, &vdn)) vd = vdec, vsz = vdn;
        if (depth < 8 && bsz >= 4 && vsz >= 4 && !memcmp(bd, "SARC", 4) && !memcmp(vd, "SARC", 4)) m = sarc_merge(bd, bsz, vd, vsz, &mn, depth + 1);
        uint32_t al = r[k].align > v[i].align ? r[k].align : v[i].align;
        if (m) {
            made(v[i].name, mn);
            uint8_t *fin = m;
            size_t fn = mn;
            if (byaz || vyaz) { fin = yaz0_store(m, mn, byaz ? r[k].data : v[i].data, &fn); free(m); }
            free(r[k].own);
            r[k].own = fin, r[k].data = fin, r[k].size = fn, r[k].align = al;
        } else {
            made_whole(v[i].name, v[i].data, v[i].size, depth + 1);
            free(r[k].own);
            r[k].own = NULL, r[k].data = v[i].data, r[k].size = v[i].size, r[k].align = v[i].align;
        }
        free(bdec), free(vdec);
    }
    uint8_t *out = sarc_write(r, nr, on);
    sarc_free(r, nr);
    sarc_free(b, nb), sarc_free(v, nv);
    return out;
}

// ---------- the resource size table ----------
static uint32_t crc32_of(const char *s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) { c ^= (uint8_t)*s; for (int k = 0; k < 8; k++) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1))); }
    return ~c;
}
enum { RNAME = 128, RNE = RNAME + 4 };
// A resource's size in a size table: by name in the collisions table, else by its name's CRC-32 (binary search).
int rstb_get(const uint8_t *t, size_t n, const char *name, uint32_t *size) {
    if (n < 12 || memcmp(t, "RSTB", 4)) return 0;
    uint32_t nc = rd32(t + 4, 0), nn = rd32(t + 8, 0);
    if (12 + (size_t)nc * 8 + (size_t)nn * RNE > n) return 0;
    const uint8_t *names = t + 12 + (size_t)nc * 8;
    for (uint32_t i = 0; i < nn; i++) if (!strncmp((const char *)names + (size_t)i * RNE, name, RNAME)) { *size = rd32(names + (size_t)i * RNE + RNAME, 0); return 1; }
    uint32_t c = crc32_of(name), lo = 0, hi = nc;
    while (lo < hi) {
        uint32_t m = (lo + hi) / 2, mc = rd32(t + 12 + (size_t)m * 8, 0);
        if (mc == c) { *size = rd32(t + 12 + (size_t)m * 8 + 4, 0); return 1; }
        if (mc < c) lo = m + 1; else hi = m;
    }
    return 0;
}
// Sets a resource's size in a size table (0 removes it), keeping the CRC-32 table sorted; the table may move.
int rstb_set(uint8_t **tp, size_t *np, const char *name, uint32_t size) {
    uint8_t *t = *tp;
    size_t n = *np;
    if (n < 12 || memcmp(t, "RSTB", 4)) return -1;
    uint32_t nc = rd32(t + 4, 0), nn = rd32(t + 8, 0);
    if (12 + (size_t)nc * 8 + (size_t)nn * RNE > n) return -1;   // a damaged table: its counts run past it
    uint8_t *names = t + 12 + (size_t)nc * 8;
    for (uint32_t i = 0; i < nn; i++)
        if (!strncmp((const char *)names + (size_t)i * RNE, name, RNAME)) {   // a name whose CRC-32 collides: kept by name
            if (size) { wr32(names + (size_t)i * RNE + RNAME, size); return 0; }
            memmove(names + (size_t)i * RNE, names + (size_t)(i + 1) * RNE, (size_t)(nn - i - 1) * RNE);
            wr32(t + 8, nn - 1), *np = n - RNE;
            return 0;
        }
    uint32_t c = crc32_of(name), lo = 0, hi = nc;
    while (lo < hi) { uint32_t m = (lo + hi) / 2; if (rd32(t + 12 + (size_t)m * 8, 0) < c) lo = m + 1; else hi = m; }
    uint8_t *at = t + 12 + (size_t)lo * 8;
    int found = lo < nc && rd32(at, 0) == c;
    if (found && size) { wr32(at + 4, size); return 0; }
    if (found) { memmove(at, at + 8, n - (size_t)(at + 8 - t)); wr32(t + 4, nc - 1), *np = n - 8; return 0; }
    if (!size) return 0;
    t = realloc(t, n + 8);
    if (!t) return -1;
    at = t + 12 + (size_t)lo * 8;
    memmove(at + 8, at, n - (size_t)(at - t));
    wr32(at, c), wr32(at + 4, size), wr32(t + 4, nc + 1);
    *tp = t, *np = n + 8;
    return 0;
}

// ---------- resource sizes (as BCML works them out) ----------
// botw rstb's resource_factory_info.tsv (public domain, leoetlino): per extension, the Switch resource class size and
// parse size, or -1: worked out another way ("complex")
static const struct { const char *ext; uint32_t size; int32_t parse; } FACT[] = {
    {"sarc", 0x68, 0}, {"pack", 0x68, 0}, {"bactorpack", 0x68, 0}, {"bmodelsh", 0x68, 0}, {"beventpack", 0x68, 0}, {"stera", 0x68, 0}, {"stats", 0x68, 0},
    {"bfres", 0x1a8, -1}, {"bcamanim", 0x50, -1}, {"batpl", 0x40, 0}, {"bnfprl", 0x40, 0}, {"bplacement", 0x48, 0}, {"hks", 0x38, 0}, {"lua", 0x38, 0},
    {"bactcapt", 0x538, 0}, {"bitemico", 0x60, 0}, {"jpg", 0x80, 0}, {"bmaptex", 0x60, 0}, {"bstftex", 0x60, 0}, {"bmapopen", 0x60, 0}, {"breviewtex", 0x60, 0},
    {"bgdata", 0x140, 0}, {"bgsvdata", 0x38, 0}, {"hknm2", 0x48, -1}, {"bmscdef", 0x2a8, -1}, {"bars", 0xb0, -1}, {"bxml", 0x778, -1},
    {"bgparamlist", 0x2c0, -1}, {"bmodellist", 0x7d0, -1}, {"baslist", 0x410, -1}, {"baiprog", 0x448, -1}, {"bphysics", 0x470, -1}, {"bchemical", 0x3c0, -1},
    {"bas", 0x3c8, -1}, {"batcllist", 0x3f0, -1}, {"batcl", 0x428, -1}, {"baischedule", 0x2b8, 0}, {"bdmgparam", 0x11d0, 0x790}, {"brgconfiglist", 0x3d0, -1},
    {"brgconfig", 0x42d8, 0}, {"brgbw", 0x2c0, -1}, {"bawareness", 0xb38, 0}, {"bdrop", 0x320, -1}, {"bshop", 0x320, -1}, {"brecipe", 0x320, -1},
    {"blod", 0x3c0, 0}, {"bbonectrl", 0x8d0, -1}, {"blifecondition", 0x4b0, -1}, {"bumii", 0x2b8, 0}, {"baniminfo", 0x2c8, -1}, {"byaml", 0x20, 0},
    {"bassetting", 0x260, -1}, {"hkrb", 0x20, 0}, {"hkrg", 0x20, 0}, {"bphyssb", 0x5b0, -1}, {"hkcl", 0xe8, -1}, {"hksc", 0x140, -1}, {"hktmrb", 0x48, -1},
    {"brgcon", 0x48, -1}, {"esetlist", 0x38, 0}, {"bdemo", 0xb20, 0}, {"bfevfl", 0x40, 0}, {"bfevtm", 0x40, 0},
};
// botw-utils' estimates (MIT-like / public domain, NiceneNerd): a multiplier by size range
typedef struct { uint32_t below; double m; } mrange;
static double pick(const mrange *r, uint32_t v) { for (; r->below; r++) if (v < r->below) return r->m; return r->m; }
static const mrange G_BAIPROG[] = {{380, 7}, {400, 6}, {450, 5.5}, {600, 5}, {1000, 4}, {1750, 3.5}, {0, 3}};
static const mrange G_BAS[] = {{100, 20}, {200, 12.5}, {300, 10}, {600, 8}, {1500, 6}, {2000, 5.5}, {15000, 5}, {0, 4.5}};
static const mrange G_BASLIST[] = {{100, 15}, {200, 10}, {300, 8}, {500, 6}, {800, 5}, {4000, 4}, {0, 3.5}};
static const mrange G_BDROP[] = {{200, 8.5}, {250, 7}, {350, 6}, {450, 5.25}, {850, 4.5}, {0, 4}};
static const mrange G_BGPARAM[] = {{100, 20}, {150, 12}, {250, 10}, {350, 8}, {450, 7}, {0, 6}};
static const mrange G_BRECIPE[] = {{100, 12.5}, {160, 8.5}, {200, 7.5}, {215, 7}, {0, 6.5}};
static const mrange G_BSHOP[] = {{200, 7.25}, {400, 6}, {500, 5}, {0, 4.05}};
static const mrange G_BXML[] = {{350, 6}, {450, 5}, {550, 4.5}, {650, 4}, {800, 3.5}, {0, 3}};
static const mrange G_TEX[] = {{10000, 2}, {30000, 1.5}, {50000, 1.3}, {0, 1.2}};
static const mrange G_MODEL[] = {{1250, 9.5}, {2500, 6}, {50000, 4.25}, {100000, 3.66}, {800000, 3.5}, {2000000, 3.15}, {3000000, 2.5}, {4000000, 1.667}, {0, 1.6}};
static int is_aamp_ext(const char *e) {   // BCML's AAMP_EXTS (with or without the "s")
    static const char *const x[] = {"bxml", "bas", "baglblm", "baglccr", "baglclwd", "baglcube", "bagldof", "baglenv", "baglenvset", "baglfila", "bagllmap",
        "bagllref", "baglmf", "baglshpp", "baiprog", "baislot", "bassetting", "batcl", "batcllist", "bawareness", "bawntable", "bbonectrl", "bchemical",
        "bchmres", "bdemo", "bdgnenv", "bdmgparam", "bdrop", "bgapkginfo", "bgapkglist", "bgenv", "bglght", "bgmsconf", "bgparamlist", "bgsdw", "bksky",
        "blifecondition", "blod", "bmodellist", "bmscdef", "bmscinfo", "bnetfp", "bphysics", "bphyssb", "bphyscharcon", "bphyscontact", "bphyslayer",
        "bphysmaterial", "bphyssubmat", "bptclconf", "brecipe", "brgbw", "brgcon", "brgconfig", "brgconfiglist", "bsfbt", "bsft", "bshop", "bumii",
        "bvege", "bactcapt", "baslist"};
    if (*e == '.') e++;
    for (size_t i = 0; i < sizeof x / sizeof *x; i++) if (!strcmp(e, x[i]) || (e[0] == 's' && !strcmp(e + 1, x[i]))) return 1;
    return 0;
}
// ext: the extension as BCML sees it (a file of the RomFS: its own, "s" and all; a file in a pack: without the "s")
static uint32_t rstb_calc(const char *path, const char *canon_name, size_t size) {
    const char *ext = ext_of(path), *ce = ext_of(canon_name);
    uint64_t r = (size + 31) & ~(size_t)31;
    const char *actual = ce[0] ? ce + 1 : "";
    int complex = 0;
    uint32_t cls = 0x38, parse = 0;   // ("*")
    for (size_t i = 0; i < sizeof FACT / sizeof *FACT; i++) if (!strcmp(actual, FACT[i].ext)) { cls = FACT[i].size; if (FACT[i].parse < 0) complex = 1; else parse = (uint32_t)FACT[i].parse; break; }
    uint64_t v = complex ? 0 : r + 0x168 + cls + parse;
    if (!strcmp(ext, ".baischedule")) v += 100;
    if (!strcmp(ext, ".bdmgparam")) v = 0;
    if (!strcmp(ext, ".hkrb")) v += 40;
    if (!strcmp(ext, ".baniminfo")) v = (uint64_t)((double)((r) * (size > 36864 ? 1.5 : 4)) + 0xE4 + 0x24C), v = (uint64_t)(v * 1.5);
    if (!v && is_aamp_ext(ext)) {   // botw-utils' guess_aamp_size
        double s = (double)size * 1.05, ret = 0;
        if (!strcmp(ext, ".bas")) s *= 1.05;
        uint32_t si = (uint32_t)s;
        const mrange *m = !strcmp(ext, ".baiprog") ? G_BAIPROG : !strcmp(ext, ".bas") ? G_BAS : !strcmp(ext, ".baslist") ? G_BASLIST : !strcmp(ext, ".bdrop") ? G_BDROP :
                          !strcmp(ext, ".bgparamlist") ? G_BGPARAM : !strcmp(ext, ".brecipe") ? G_BRECIPE : !strcmp(ext, ".bshop") ? G_BSHOP : !strcmp(ext, ".bxml") ? G_BXML : NULL;
        if (m) ret = si * pick(m, si);
        else if (!strcmp(ext, ".bdmgparam")) ret = ((-0.0018 * si) + 6.6273) * si + 500;
        else if (!strcmp(ext, ".bphysics")) { double k = 4 * floor(si / 1388.0); ret = (double)(((si + 32) & ~31u) + 0x4E + 0x324) * (k > 3 ? k : 3); }
        v = (uint64_t)(ret * 1.5);
    } else if (!v && (!strcmp(ext, ".bfres") || !strcmp(ext, ".sbfres"))) {   // botw-utils' guess_bfres_size
        uint32_t si = (uint32_t)((double)size * 1.05);
        v = (uint64_t)(si * pick(strstr(path, ".Tex") ? G_TEX : G_MODEL, si));
    }
    return v > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)v;
}
static int rstb_excluded(const char *c) {   // BCML's EXCLUDE_EXTS and EXCLUDE_NAMES
    static const char *const x[] = {".pack", ".bgdata", ".txt", ".bgsvdata", ".yml", ".msbt", ".bat", ".ini", ".png", ".bfstm", ".py", ".sh"};
    if (!strcmp(c, "Actor/ActorInfo.product.byml") || !strcmp(c, "System/Resource/ResourceSizeTable.product.rsizetable")) return 1;
    for (size_t i = 0; i < sizeof x / sizeof *x; i++) if (!strcmp(ext_of(c), x[i])) return 1;
    return 0;
}
typedef struct { char name[512]; uint32_t size; } rsz_t;
static void rsz_put(rsz_t **d, int *n, const char *name, uint32_t size) {   // (a later one replaces an earlier)
    for (int i = 0; i < *n; i++) if (!strcmp((*d)[i].name, name)) { (*d)[i].size = size; return; }
    *d = realloc(*d, (size_t)(*n + 1) * sizeof **d);
    snprintf((*d)[*n].name, sizeof (*d)[0].name, "%s", name);
    (*d)[(*n)++].size = size;
}

// ---------- the merge ----------
typedef struct { char *rel; int layer; } fent;
typedef struct { char romfs[1300], aoc[1300], logs[1300]; } layer_t;   // aoc: the layer's DLC files
// Lists the files under a folder (paths relative to it, up to 32 levels), each with the layer it is in.
static void walk(const char *root, const char *rel, int layer, fent **f, int *n, int *cap, int depth) {
    char p[2600];
    snprintf(p, sizeof p, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(p);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char r[2048], full[2600];
        snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", e->d_name);
        snprintf(full, sizeof full, "%s/%s", root, r);
        if (is_dir(full)) { if (depth < 32) walk(root, r, layer, f, n, cap, depth + 1); continue; }
        if (*n == *cap) *f = realloc(*f, (size_t)(*cap = *cap ? *cap * 2 : 256) * sizeof **f);
        (*f)[*n].rel = strdup(r), (*f)[(*n)++].layer = layer;
    }
    if (d) closedir(d);
}
static int by_rel(const void *a, const void *b) {
    const fent *x = a, *y = b;
    int c = strcmp(x->rel, y->rel);
    return c ? c : x->layer - y->layer;
}
static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static char *romfs_rel(const char *v) {   // "01007EF00011E000\\romfs\\Actor\\Pack\\X.sbactorpack": "Actor/Pack/X.sbactorpack"
    char *s = strdup(v);
    for (char *c = s; *c; c++) if (*c == '\\') *c = '/';
    char *r = strstr(s, "romfs/");
    if (r) memmove(s, r + 6, strlen(r + 6) + 1);
    return s;
}
static void canon(const char *rel, char *out, size_t n) {   // the name the size table knows: no "s" for Yaz0 before the extension
    snprintf(out, n, "%s", rel);
    char *dot = strrchr(out, '.'), *sl = strrchr(out, '/');
    if (dot && (!sl || dot > sl) && dot[1] == 's' && strlen(dot) > 2) memmove(dot + 1, dot + 2, strlen(dot + 2) + 1);
}
// ---------- BCML's AAMP merges (logs/deepmerge.aamp, logs/aslist.aamp) ----------
// BCML leaves a mod's changed parameter files (AAMP: .bxml, .bmodellist, .baslist ...) out of its packs and logs what
// it changes instead: a document whose FileTable object lists the files ("<content>/Actor/Pack/X.sbactorpack//Actor/
// ModelList/X.bmodellist", "//" going into a pack), and a list per file, named by its path, of the changes. They are
// merged into the file in the pack the merge makes (or the game's own pack when the mod has none of it).
typedef struct { char *top, *rest; const aamp_list *diff; int aslist, done, aoc; } edit_t;   // aoc: a DLC file
typedef struct { edit_t *e; int n; aamp_pio *docs; int *asl, nd; int dlc, bad, applied, text, tree; } edits_t;   // tree: the files being merged (1: the DLC's)   // asl: the document is aslist.aamp
// Reads a layer's AAMP logs (deepmerge.aamp, aslist.aamp).
static void edits_load(edits_t *E, const char *logs) {
    static const char *const names[2] = {"deepmerge.aamp", "aslist.aamp"};
    for (int k = 0; k < 2; k++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/%s", logs, names[k]);
        size_t n;
        uint8_t *b = slurp(p, &n);
        if (!b) continue;
        aamp_pio d;
        if (aamp_read(b, n, &d)) { if (n >= 4 && memcmp(b, "AAMP", 4)) E->text++; else E->bad++; free(b); continue; }   // (BCML writes it as YAML text when it cannot be binary)
        free(b);
        E->docs = realloc(E->docs, (size_t)(E->nd + 1) * sizeof *E->docs);
        E->asl = realloc(E->asl, (size_t)(E->nd + 1) * sizeof *E->asl);
        E->asl[E->nd] = k == 1;
        E->docs[E->nd++] = d;
    }
}
static void edits_index(edits_t *E, int from) {   // the files of the documents loaded since from
    for (int di = from; di < E->nd; di++) {
        const aamp_list *root = &E->docs[di].root;
        const aamp_obj *ft = aamp_find_obj(root, aamp_crc("FileTable"));
        for (int i = 0; ft && i < ft->np; i++) {
            char *path = aamp_str(&ft->p[i]);
            if (!path) continue;
            const aamp_list *diff = aamp_find_list(root, aamp_crc(path));
            for (char *c = path; *c; c++) if (*c == '\\') *c = '/';
            char *sep = strstr(path, "//"), *rest = NULL;
            if (sep) *sep = 0, rest = strdup(sep + 2);
            char *r = strstr(path, "romfs/"), low[64];
            snprintf(low, sizeof low, "%.*s", r ? (int)(r - path) < 63 ? (int)(r - path) : 63 : 63, path);
            for (char *c = low; *c; c++) *c = (char)tolower((unsigned char)*c);
            if (!diff) { E->bad++; free(path), free(rest); continue; }
            int aoc = !strncasecmp(path, "aoc", 3) || strstr(low, "11f0") || strstr(low, "/aoc");   // a DLC file
            const char *top = r ? r + 6 : !strncmp(path, "content/", 8) ? path + 8 : path;
            if (aoc && !r) { top = path; for (int k = 0; k < 2 && strchr(top, '/'); k++) if (!strncasecmp(top, "aoc/", 4) || !strncmp(top, "0010/", 5) || !strncmp(top, "content/", 8)) top = strchr(top, '/') + 1; }
            E->e = realloc(E->e, (size_t)(E->n + 1) * sizeof *E->e);
            E->e[E->n++] = (edit_t){strdup(top), rest, diff, E->asl[di], 0, aoc};
            free(path);
        }
    }
}
static void edits_free(edits_t *E) {
    for (int i = 0; i < E->n; i++) free(E->e[i].top), free(E->e[i].rest);
    for (int i = 0; i < E->nd; i++) aamp_free(&E->docs[i]);
    free(E->e), free(E->docs), free(E->asl);
}
static int edits_for(const edits_t *E, const char *rel) { for (int i = 0; i < E->n; i++) if (!E->e[i].done && E->e[i].aoc == E->tree && !strcmp(E->e[i].top, rel)) return 1; return 0; }
// Merges a logged change into an AAMP file; the file written again, or NULL when it does not read.
static uint8_t *aamp_apply(const uint8_t *d, size_t n, const edit_t *e, size_t *on) {
    aamp_pio p;
    if (aamp_read(d, n, &p)) return NULL;
    aamp_merge(&p.root, e->diff, e->aslist);
    uint8_t *o = aamp_write(&p, on);
    aamp_free(&p);
    return o;
}
// the file rest ("A.sbactorpack//B.bxml": into the pack inside) of the SARC d changed; NULL when it is not there
static uint8_t *apply_in(const uint8_t *d, size_t n, const char *rest, const edit_t *e, size_t *on, int depth) {
    const char *sep = strstr(rest, "//");
    char name[512];
    snprintf(name, sizeof name, "%.*s", sep ? (int)(sep - rest) : (int)strlen(rest), rest);
    sarc_ent *ent;
    int k = depth < 8 ? sarc_read(d, n, &ent) : -1, i = 0;
    if (k < 0) return NULL;
    while (i < k && strcmp(ent[i].name, name)) i++;
    if (i == k) { sarc_free(ent, k); return NULL; }
    const uint8_t *ed = ent[i].data;
    size_t en = ent[i].size, nn = 0;
    uint8_t *dec = NULL, *nw;
    int yaz = en >= 16 && !memcmp(ed, "Yaz0", 4);
    if (yaz) { size_t dn; if (yaz0_decode(ed, en, &dec, &dn)) { sarc_free(ent, k); return NULL; } ed = dec, en = dn; }
    nw = sep ? apply_in(ed, en, sep + 2, e, &nn, depth + 1) : aamp_apply(ed, en, e, &nn);
    uint8_t *o = NULL;
    if (nw) {
        made(name, nn);
        if (yaz) { size_t yn; uint8_t *y = yaz0_store(nw, nn, ent[i].data, &yn); free(nw); nw = y, nn = yn; }
        free(ent[i].own);
        ent[i].own = nw, ent[i].data = nw, ent[i].size = nn;
        o = sarc_write(ent, k, on);
    }
    sarc_free(ent, k);
    free(dec);
    return o;
}
// every change logged for the file rel made in *cur (decompressed)
static void apply_edits(edits_t *E, const char *rel, uint8_t **cur, size_t *cn) {
    for (int i = 0; i < E->n; i++) {
        edit_t *e = &E->e[i];
        if (e->done || e->aoc != E->tree || strcmp(e->top, rel)) continue;
        e->done = 1;
        size_t nn = 0;
        uint8_t *nw = !*cur ? NULL : e->rest ? apply_in(*cur, *cn, e->rest, e, &nn, 0) : aamp_apply(*cur, *cn, e, &nn);
        if (!nw) { E->bad++; fprintf(stderr, "[NativeNX] BCML mods: %s%s%s: not found in the game's files, its changes left out\n", rel, e->rest ? "//" : "", e->rest ? e->rest : ""); continue; }
        free(*cur);
        *cur = nw, *cn = nn;
        E->applied++;
        if (e->aoc) E->dlc++;
    }
}

// one tree of files merged (tree 0: the game's, into ro; 1: the DLC's): the layers' files and packs over the game's,
// the AAMP logs' changes in them; 0 on success
#define ROOT(i) (tree ? L[i].aoc : L[i].romfs)
// ---------- the merge of one tree of files ----------
// A file's bytes decompressed when they are Yaz0 (raw is freed then): *yaz says it was, hdr keeps its header (when hdr is
// not yet set); NULL when it does not decompress
static uint8_t *unyaz(uint8_t *raw, size_t rn, size_t *on, int *yaz, uint8_t hdr[16], int *hdr_set) {
    if (rn < 16 || memcmp(raw, "Yaz0", 4)) { *on = rn; return raw; }
    *yaz = 1;
    if (!*hdr_set) memcpy(hdr, raw, 16), *hdr_set = 1;
    uint8_t *d = NULL;
    if (yaz0_decode(raw, rn, &d, on)) d = NULL;
    free(raw);
    return d;
}
// Writes a merged file (Yaz0 again when it was) at ro/rel and notes its size; cur is freed; 0 on success
static int put_merged(const char *ro, const char *rel, uint8_t *cur, size_t cn, int yaz, const uint8_t *hdr, char *err, size_t en) {
    char dst[2600];
    snprintf(dst, sizeof dst, "%s/%s", ro, rel);
    uint8_t *fin = cur;
    size_t fn = cn;
    if (cur && yaz) fin = yaz0_store(cur, cn, hdr, &fn);
    unlink(dst);
    int r = !fin || write_file(dst, fin, fn) ? fail(err, en, "could not write %s", dst) : 0;
    made_top(rel, cn, 1);
    if (fin != cur) free(fin);
    free(cur);
    return r;
}
// The packs this tree's files are merged with the game's own (the packs.json of every layer); sorted, *n of them
static char **packs_of(layer_t *L, int nl, int tree, int *n) {
    char **packs = NULL;
    *n = 0;
    for (int i = 0; i < nl; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/packs.json", L[i].logs);
        jv j;
        if (jload(p, &j)) continue;
        for (int k = 0; j.t == 5 && k < j.n; k++) {
            if (j.v[k].t != 3) continue;
            int aoc = strcasestr(j.v[k].s, "11f0") || !strncasecmp(j.k[k], "Aoc/", 4);
            if (aoc != tree) continue;
            packs = realloc(packs, (size_t)(*n + 1) * sizeof *packs);
            packs[(*n)++] = romfs_rel(j.v[k].s);
        }
        jfree(&j);
    }
    if (*n) qsort(packs, (size_t)*n, sizeof *packs, by_str);
    return packs;
}
#define ROOT(i) (tree ? L[i].aoc : L[i].romfs)
// A file to merge (a pack the mods change, or one the AAMP logs change): the game's own pack (when it is one), each
// layer's over it from the lowest, then the logged changes; f[i..j) are the layers that have it
static int merge_file(layer_t *L, int tree, const fent *f, int i, int j, int pack, bnp_reader rd, void *ctx, const char *ro, edits_t *E, char *err, size_t en) {
    const char *rel = f[i].rel;
    uint8_t *cur = NULL, hdr[16] = {0}, *raw;
    size_t cn = 0, rn;
    int yaz = 0, hdr_set = 0;
    if (pack && !rd(ctx, rel, &raw, &rn)) cur = unyaz(raw, rn, &cn, &yaz, hdr, &hdr_set);   // the game's own pack first
    for (int k = pack ? i : j - 1; k < j; k++) {   // (a file not in packs.json: the top layer's, as it is)
        char src[2600];
        snprintf(src, sizeof src, "%s/%s", ROOT(f[k].layer), rel);
        uint8_t *m = slurp(src, &rn);
        size_t mn;
        if (!m || !(m = unyaz(m, rn, &mn, &yaz, hdr, &hdr_set))) continue;
        if (!cur) { cur = m, cn = mn; continue; }
        size_t gn;
        uint8_t *mg = sarc_merge(cur, cn, m, mn, &gn, 0);
        if (mg) { free(cur); cur = mg, cn = gn; free(m); }
        else { free(cur); cur = m, cn = mn; }   // not a SARC after all: the mod's
    }
    apply_edits(E, rel, &cur, &cn);
    return put_merged(ro, rel, cur, cn, yaz, hdr, err, en);
}
// A file the top layer has, taken as it is: linked (or copied), and its size noted, and its files' when it is a pack
static int link_file(const char *src, const char *ro, const char *rel, char *err, size_t en) {
    char dst[2600], dir[2600], abs[PATH_MAX];
    snprintf(dst, sizeof dst, "%s/%s", ro, rel);
    snprintf(dir, sizeof dir, "%s", dst);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    unlink(dst);
    size_t sn = 0;
    uint8_t *b = slurp(src, &sn);
    int r = 0;
    if (symlink(realpath(src, abs) ? abs : src, dst) && (!b || write_file(dst, b, sn))) r = fail(err, en, "could not write %s", dst);   // (a link that works from anywhere)
    if (b) {
        made_top(rel, real_size(b, sn), 1);
        char c[512];
        canon(rel, c, sizeof c);
        if (sarc_ext(ext_of(c))) {
            uint8_t *d = b, *dec = NULL;
            size_t dn = sn;
            int yaz = sn >= 16 && !memcmp(b, "Yaz0", 4);
            if (yaz && !yaz0_decode(b, sn, &dec, &dn)) d = dec;
            sarc_ent *e;
            int k = yaz && !dec ? -1 : sarc_read(d, dn, &e);
            for (int q = 0; q < k; q++) made_whole(e[q].name, e[q].data, e[q].size, 1);
            if (k >= 0) sarc_free(e, k);
            free(dec);
        }
        free(b);
    }
    return r;
}
// The logged changes to files the mods do not have: made in the game's own files
static int edit_game_files(int tree, bnp_reader rd, void *ctx, const char *ro, edits_t *E, int *nmerge, char *err, size_t en) {
    int r = 0;
    for (int i = 0; i < E->n && !r; i++) {
        if (E->e[i].done || E->e[i].aoc != tree) continue;
        const char *rel = E->e[i].top;
        uint8_t *raw, *cur = NULL, hdr[16];
        size_t rn, cn = 0;
        int yaz = 0, hdr_set = 0;
        if (rd(ctx, rel, &raw, &rn)) { apply_edits(E, rel, &cur, &cn); continue; }   // (counted as not found)
        cur = unyaz(raw, rn, &cn, &yaz, hdr, &hdr_set);
        int before = E->applied;
        apply_edits(E, rel, &cur, &cn);
        if (cur && E->applied > before) { r = put_merged(ro, rel, cur, cn, yaz, hdr, err, en); (*nmerge)++; }
        else free(cur);
    }
    return r;
}
// Merges one tree of files (the game's, or the DLC's) of every layer into ro: packs with the game's, other files as
// the top layer has them, the AAMP logs into their files
static int merge_tree(layer_t *L, int nl, int tree, bnp_reader rd, void *ctx, const char *ro, edits_t *E, int *nmerge, int *nlink, char *err, size_t en) {
    E->tree = tree;
    fent *f = NULL;
    int nf = 0, cap = 0, np, r = 0;
    for (int i = 0; i < nl; i++) if (ROOT(i)[0]) walk(ROOT(i), "", i, &f, &nf, &cap, 0);
    if (nf) qsort(f, (size_t)nf, sizeof *f, by_rel);
    char **packs = packs_of(L, nl, tree, &np);
    mkdirs(ro);
    for (int i = 0; i < nf && !r;) {
        int j = i;
        while (j < nf && !strcmp(f[j].rel, f[i].rel)) j++;   // [i, j): the layers that have this file, lowest first
        const char *rel = f[i].rel;
        int pack = np && bsearch(&rel, packs, (size_t)np, sizeof *packs, by_str) != NULL;
        if (pack || edits_for(E, rel)) {
            r = merge_file(L, tree, f, i, j, pack, rd, ctx, ro, E, err, en);
            if (pack) (*nmerge)++; else (*nlink)++;
        } else {
            char src[2600];
            snprintf(src, sizeof src, "%s/%s", ROOT(f[j - 1].layer), rel);
            r = link_file(src, ro, rel, err, en);
            (*nlink)++;
        }
        i = j;
    }
    if (!r) r = edit_game_files(tree, rd, ctx, ro, E, nmerge, err, en);
    for (int i = 0; i < nf; i++) free(f[i].rel);
    for (int i = 0; i < np; i++) free(packs[i]);
    free(f), free(packs);
    return r;
}
#undef ROOT

// ---------- the layers, the size table, the whole merge ----------
// The layers of the merge, lowest first: each mod, then its options turned on (over it); their count in *nl
static layer_t *layers_make(const bnp_t *mods, const uint32_t *on, int nmods, int *nl) {
    layer_t *L = calloc((size_t)nmods * 17 + 1, sizeof *L);
    *nl = 0;
    for (int m = 0; m < nmods; m++) {
        if (mods[m].romfs[0] || mods[m].aoc[0]) {
            layer_t *l = &L[(*nl)++];
            snprintf(l->romfs, sizeof l->romfs, "%s", mods[m].romfs);
            snprintf(l->aoc, sizeof l->aoc, "%s", mods[m].aoc);
            snprintf(l->logs, sizeof l->logs, "%s/logs", mods[m].dir);
        }
        for (int k = 0; k < mods[m].nopt; k++) {
            if (!(on[m] >> k & 1)) continue;
            layer_t *l = &L[(*nl)++];
            char od[1300];
            snprintf(od, sizeof od, "%s/options/%s", mods[m].dir, mods[m].opt[k].folder);
            if (!find_romfs(od, l->romfs, sizeof l->romfs)) l->romfs[0] = 0;
            if (!find_romfs_of(od, 1, l->aoc, sizeof l->aoc)) l->aoc[0] = 0;
            snprintf(l->logs, sizeof l->logs, "%s/logs", od);
        }
    }
    return L;
}
typedef struct { int set, removed, lower; } rstb_counts;
// The sizes for the table, as BCML's RstbMerger decides them: the sizes the layers logged (rstb.json), then those of
// the files the merge made and the mods brought, worked out (rstb_calc); a computed size below the game's is not used
static rsz_t *sizes_decide(layer_t *L, int nl, const uint8_t *tab, size_t tn, int *nd) {
    rsz_t *dz = NULL;
    *nd = 0;
    for (int i = 0; i < nl; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s/rstb.json", L[i].logs);
        jv j;
        if (jload(p, &j)) continue;
        for (int k = 0; j.t == 5 && k < j.n; k++) if (j.v[k].t == 2 && j.v[k].num >= 0 && j.v[k].num < 4e9) rsz_put(&dz, nd, j.k[k], (uint32_t)j.v[k].num);
        jfree(&j);
    }
    for (int i = 0; i < g_nmade; i++) {
        char c[512];
        canon(g_made[i].name, c, sizeof c);
        uint32_t v = rstb_calc(g_made[i].top ? g_made[i].name : c, c, g_made[i].size), have;
        if (rstb_excluded(c)) continue;
        int in = rstb_get(tab, tn, c, &have);
        if (in ? v && v < have : !v) continue;
        rsz_put(&dz, nd, c, v);
    }
    return dz;
}
// Sets the sizes in the table: 0 removes an entry, a size below the game's own is not used; each decision written to
// <out>/sizes.txt (for finding out why a mod does not load), with the real size of the files made
static void sizes_apply(uint8_t **tab, size_t *tn, const rsz_t *dz, int nd, const char *out, rstb_counts *c) {
    char sp[1400];
    snprintf(sp, sizeof sp, "%s/sizes.txt", out);
    FILE *sf = fopen(sp, "w");
    if (sf) fprintf(sf, "# resource size table: name, the game's size (- none), the size now (- removed), made files' real size\n");
    for (int i = 0; i < nd; i++) {
        uint32_t have = 0, now;
        int in = rstb_get(*tab, *tn, dz[i].name, &have);
        const char *what;
        if (!dz[i].size) { now = 0, what = in ? "removed" : "none"; if (in) rstb_set(tab, tn, dz[i].name, 0), c->removed++; }
        else if (in && dz[i].size < have) now = have, what = "game's kept", c->lower++;
        else now = dz[i].size, what = "set", rstb_set(tab, tn, dz[i].name, dz[i].size), c->set++;
        if (!sf) continue;
        long real = -1;
        for (int k = g_nmade - 1; k >= 0 && real < 0; k--) { char cn[512]; canon(g_made[k].name, cn, sizeof cn); if (!strcmp(cn, dz[i].name)) real = (long)g_made[k].size; }
        char a[16] = "-", b[16] = "-";
        if (in) snprintf(a, sizeof a, "%u", have);
        if (now) snprintf(b, sizeof b, "%u", now);
        fprintf(sf, "%s\t%s\t%s\t%s (wanted %u)", dz[i].name, a, b, what, dz[i].size);
        if (real >= 0) fprintf(sf, "\tfile %ld", real);
        fprintf(sf, "\n");
    }
    if (sf) {
        fprintf(sf, "# made files (name, real size)\n");
        for (int k = 0; k < g_nmade; k++) fprintf(sf, "%s\t%zu%s\n", g_made[k].name, g_made[k].size, g_made[k].top ? "\ttop" : "");
        fclose(sf);
    }
}
// The game's size table with the layers' sizes, written into ro (Yaz0, as the game's); 0 on success
static int size_table_write(layer_t *L, int nl, bnp_reader rd, void *ctx, const char *out, const char *ro, rstb_counts *c, char *err, size_t en) {
    static const char *const RS = "System/Resource/ResourceSizeTable.product.srsizetable";
    uint8_t *raw, *tab = NULL, hdr[16] = {0};
    size_t rn, tn = 0;
    int yaz = 0, hdr_set = 0, r = 0;
    if (rd(ctx, RS, &raw, &rn)) { fprintf(stderr, "[NativeNX] BCML mods: the game has no resource size table (%s): none written\n", RS); return 0; }
    if (!(tab = unyaz(raw, rn, &tn, &yaz, hdr, &hdr_set))) return 0;
    int nd;
    rsz_t *dz = sizes_decide(L, nl, tab, tn, &nd);
    sizes_apply(&tab, &tn, dz, nd, out, c);
    free(dz);
    size_t yn;
    uint8_t *y = yaz0_store(tab, tn, hdr_set ? hdr : NULL, &yn);
    char dst[1400];
    snprintf(dst, sizeof dst, "%s/%s", ro, RS);
    unlink(dst);
    if (!y || write_file(dst, y, yn)) r = fail(err, en, "could not write the size table");
    free(y), free(tab);
    return r;
}
// Merges the mods into out/romfs (and their DLC files into out/aoc), then writes the size table as BCML would.
int bnp_build(const bnp_t *mods, const uint32_t *on, int nmods, bnp_reader rd, void *ctx, bnp_reader rd_aoc, void *ctx_aoc, const char *out, char *err, size_t en) {
    int nl;
    layer_t *L = layers_make(mods, on, nmods, &nl);
    free(g_made);
    g_made = NULL, g_nmade = 0;
    edits_t E = {0};
    for (int i = 0; i < nl; i++) {   // the layers' AAMP changes, in their order (an option's after its mod's)
        int from = E.nd;
        edits_load(&E, L[i].logs);
        edits_index(&E, from);
    }
    int nmerge = 0, nlink = 0, have_aoc = 0, dlc_left = 0;
    char ro[1300];
    snprintf(ro, sizeof ro, "%s/romfs", out);
    int r = merge_tree(L, nl, 0, rd, ctx, ro, &E, &nmerge, &nlink, err, en);
    for (int i = 0; i < nl; i++) if (L[i].aoc[0]) have_aoc = 1;
    for (int i = 0; i < E.n; i++) if (E.e[i].aoc) have_aoc = 1;
    if (!r && have_aoc && rd_aoc) {   // the DLC's files: the same, over the DLC's own (named "Aoc/0010/..." in the size table)
        char ao[1300];
        snprintf(ao, sizeof ao, "%s/aoc", out);
        g_prefix = "Aoc/0010/";
        r = merge_tree(L, nl, 1, rd_aoc, ctx_aoc, ao, &E, &nmerge, &nlink, err, en);
        g_prefix = "";
    } else if (have_aoc) dlc_left = 1;
    rstb_counts c = {0};
    if (!r) r = size_table_write(L, nl, rd, ctx, out, ro, &c, err, en);
    if (!r) {
        fprintf(stderr, "[NativeNX] BCML mods: %d pack%s merged with the game's, %d file%s as they are; sizes: %d set, %d removed, %d kept at the game's (larger)\n", nmerge, nmerge == 1 ? "" : "s", nlink, nlink == 1 ? "" : "s", c.set, c.removed, c.lower);
        if (dlc_left) fprintf(stderr, "[NativeNX] BCML mods: the mods change DLC files, but the game has no DLC turned on: those changes left out\n");
        if (E.applied || E.bad || E.dlc || E.text)
            fprintf(stderr, "[NativeNX] BCML mods: parameter files: %d changed as the logs say (%d of them in DLC files), %d not found%s\n", E.applied, E.dlc, E.bad,
                    E.text ? "; a log written as text, not read" : "");
    }
    edits_free(&E);
    free(L);
    return r;
}
