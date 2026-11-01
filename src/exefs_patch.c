// SPDX-License-Identifier: GPL-3.0-or-later
// Code patches of mods (a mod's exefs folder, as on a modded console or in Switch emulators): IPS / IPS32 files named by
// the module's build ID (Atmosphere's exefs_patches layout: <build ID>.ips), and IPSwitch text patches (*.pchtxt) that
// name their module inside (@nsobid-<build ID>). NXL_EXEFS_PATCHES=<folder>[:<folder>...] (';' between them on Windows) lists the folders (NativeNX
// passes the exefs folders of the mods turned on); each module loaded gets the patches made for its build. Offsets in
// both are from the start of the NSO file's 0x100-byte header (a pchtxt's @flag offset_shift is added first), so the
// image offset is 0x100 less. Applied after nxnative's cuts and frame step, before the module is relocated
#include "nxl.h"
#include "platform.h"
#include <ctype.h>
#include <dirent.h>
#include <strings.h>

#define NSO_HEADER 0x100
// Writes patch bytes into a module's code (in its NSO image: offsets count from its header); 0 when they fall outside.
static int put(module_t *m, uint64_t off, const uint8_t *b, size_t n, const char *from, unsigned *bytes) {
    if (off < NSO_HEADER || off - NSO_HEADER + n > m->size) {
        RED("%s: a patch at %#llx (%zu bytes) is outside %s's image: left out", from, (unsigned long long)off, n, m->name);
        return 0;
    }
    uint64_t at = off - NSO_HEADER;
    memcpy(m->base + at, b, n);
    if (nxl_verbose > 1) nxl_log("[nxl] ", "%s+%#llx: %zu bytes from %s", m->name, (unsigned long long)at, n, from);
    *bytes += (unsigned)n;
    return 1;
}
static int hexval(int c) { return isdigit(c) ? c - '0' : isxdigit(c) ? (tolower(c) - 'a' + 10) : -1; }
static void bid_hex(const module_t *m, char out[65]) { for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", m->build_id[i]); }
// the build ID a patch names matches the module's: as many hex digits as it gives (Atmosphere trims trailing zeros)
static int bid_matches(const module_t *m, const char *want) {
    char have[65];
    bid_hex(m, have);
    size_t n = 0;
    while (want[n] && isxdigit((unsigned char)want[n]) && n < 64) n++;
    if (n < 16) return 0;
    for (size_t i = 0; i < n; i++) if (tolower((unsigned char)want[i]) != have[i]) return 0;
    for (size_t i = n; i < 64; i++) if (have[i] != '0' && n < 32) return 0;   // a shorter name stands for trailing zeros
    return 1;
}
// Applies an IPS / IPS32 patch file to a module.
static void apply_ips(module_t *m, const char *path, unsigned *np, unsigned *nb) {
    size_t sz;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long fl = ftell(f);
    if (fl < 5) { fclose(f); RED("%s: not an IPS file", path); return; }
    sz = (size_t)fl;
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(sz + 1);
    if (fread(d, 1, sz, f) != sz) { fclose(f); free(d); return; }
    fclose(f);
    int ips32 = sz >= 5 && !memcmp(d, "IPS32", 5);
    size_t p = ips32 ? 5 : 5, ob = ips32 ? 4 : 3;
    if (sz < 5 || (!ips32 && memcmp(d, "PATCH", 5))) { RED("%s: not an IPS file", path); free(d); return; }
    while (p + ob <= sz) {
        if (!ips32 && !memcmp(d + p, "EOF", 3)) break;
        if (ips32 && p + 4 <= sz && !memcmp(d + p, "EEOF", 4)) break;
        uint64_t off = 0;
        for (size_t i = 0; i < ob; i++) off = off << 8 | d[p + i];
        p += ob;
        if (p + 2 > sz) break;
        size_t n = (size_t)d[p] << 8 | d[p + 1];
        p += 2;
        if (n == 0) {   // a run of one byte
            if (p + 3 > sz) break;
            size_t rn = (size_t)d[p] << 8 | d[p + 1];
            uint8_t *run = malloc(rn ? rn : 1);
            memset(run, d[p + 2], rn);
            *np += put(m, off, run, rn, path, nb);
            free(run);
            p += 3;
        } else {
            if (p + n > sz) break;
            *np += put(m, off, d + p, n, path, nb);
            p += n;
        }
    }
    free(d);
}
// Applies a pchtxt patch file (an offset and hex bytes per line, for one build) to a module.
static void apply_pchtxt(module_t *m, const char *path, unsigned *np, unsigned *nb) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[4096];
    int matched = -1, on = 0, le = 0;
    uint64_t shift = 0;
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        char *c = strstr(s, "//");
        if (c) *c = 0;
        size_t L = strlen(s);
        while (L && isspace((unsigned char)s[L - 1])) s[--L] = 0;
        if (!*s || *s == '#') continue;
        if (!strncasecmp(s, "@nsobid-", 8)) { matched = bid_matches(m, s + 8); if (!matched) break; continue; }
        if (!strncasecmp(s, "@flag offset_shift", 18)) { shift = strtoull(s + 18, NULL, 0); continue; }
        if (!strcasecmp(s, "@little-endian") || !strcasecmp(s, "@flag le")) { le = 1; continue; }
        if (!strcasecmp(s, "@big-endian") || !strcasecmp(s, "@flag be")) { le = 0; continue; }
        if (!strncasecmp(s, "@enabled", 8)) { on = 1; continue; }
        if (!strncasecmp(s, "@disabled", 9)) { on = 0; continue; }
        if (!strncasecmp(s, "@stop", 5)) break;
        if (*s == '@' || !on || matched != 1) continue;
        // OFFSET VALUE: hex bytes as written, or "a string"
        char *e;
        uint64_t off = strtoull(s, &e, 16);
        if (e == s) continue;
        while (isspace((unsigned char)*e)) e++;
        uint8_t b[2048];
        size_t n = 0;
        if (*e == '"') {
            for (e++; *e && *e != '"' && n < sizeof b; e++) b[n++] = (uint8_t)*e;
        } else {
            int hi = -1;
            for (; *e && n < sizeof b; e++) {   // hex digits, pairs to bytes, spaces between allowed
                if (isspace((unsigned char)*e)) continue;
                int v = hexval((unsigned char)*e);
                if (v < 0) break;
                if (hi < 0) hi = v; else b[n++] = (uint8_t)(hi << 4 | v), hi = -1;
            }
            if (le) for (size_t i = 0; i < n / 2; i++) { uint8_t t = b[i]; b[i] = b[n - 1 - i]; b[n - 1 - i] = t; }
        }
        if (n) *np += put(m, off + shift, b, n, path, nb);
    }
    fclose(f);
}
// Applies the mods' code patches (NXL_EXEFS_PATCHES folders) whose build ID is the module's.
void exefs_patches_apply(module_t *m) {
    const char *e = getenv("NXL_EXEFS_PATCHES");
    if (!e || !*e) return;
    char *list = strdup(e), *save = NULL;
    for (char *dir = strtok_r(list, NXL_LIST_SEP, &save); dir; dir = strtok_r(NULL, NXL_LIST_SEP, &save)) {
        DIR *d = opendir(dir);
        if (!d) { RED("NXL_EXEFS_PATCHES: %s is not a folder", dir); continue; }
        struct dirent *de;
        while ((de = readdir(d))) {
            size_t l = strlen(de->d_name);
            char path[2048];
            snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
            unsigned np = 0, nb = 0;
            if (l > 4 && !strcasecmp(de->d_name + l - 4, ".ips")) {
                char name[80];
                snprintf(name, sizeof name, "%.*s", (int)(l - 4), de->d_name);
                if (bid_matches(m, name)) apply_ips(m, path, &np, &nb);
            } else if (l > 7 && !strcasecmp(de->d_name + l - 7, ".pchtxt")) apply_pchtxt(m, path, &np, &nb);
            if (np) LOG("%s: a mod's code patch %s: %u places, %u bytes", m->name, path, np, nb);
        }
        closedir(d);
    }
    free(list);
}
