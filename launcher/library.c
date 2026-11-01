// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's game library (library.h)
#define _GNU_SOURCE
#include "library.h"
#include "paths.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
// a child of dir whose name matches one of names, ignoring case (RomFS / romfs, ExeFS / exefs)
static int find_child(const char *dir, const char *const *names, int want_dir, char *out, size_t n) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        for (int i = 0; names[i] && !found; i++) {
            if (strcasecmp(e->d_name, names[i])) continue;
            snprintf(out, n, "%s/%s", dir, e->d_name);
            found = want_dir ? is_dir(out) : is_file(out);
        }
    }
    closedir(d);
    return found;
}

// ---------- the console's title data ----------
// NACP: 16 title entries of 0x300 bytes (name 0x200, publisher 0x100), by language: 0 American English, 1 British
// English, 2 Japanese, 3 French, 4 German, 5 Latin American Spanish, 6 Spanish, 7 Italian, 8 Dutch, 9 Canadian French,
// 10 Portuguese, 11 Russian, 12 Korean, 13 Traditional Chinese, 14 Simplified Chinese, 15 Brazilian Portuguese;
// the display version at 0x3060 (16 bytes)
// stb_image's zlib decoder (its implementation is in ui_draw.c): raw Deflate, and with a zlib header
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
int stbi_zlib_decode_buffer(char *obuffer, int olen, const char *ibuffer, int ilen);
// text the launcher can show: valid UTF-8 without control characters
static int good_text(const char *s, size_t max) {
    size_t i = 0;
    while (i < max && s[i]) {
        uint8_t c = (uint8_t)s[i];
        int n = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0 || (n == 0 && c < 0x20)) return 0;
        for (int k = 1; k <= n; k++) if (i + k >= max || ((uint8_t)s[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)n + 1;
    }
    return i > 0;
}
// Reads a game's title, publisher and version from its control.nacp (the first language that has a title); 1 when it
// read.
int nacp_read(const char *path, char *title, size_t tn, char *publisher, size_t pn, char *version, size_t vn) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t b[0x4000];
    size_t n = fread(b, 1, sizeof b, f);
    fclose(f);
    if (n < 0x3216) return 0;
    // The titles: 16 entries of 0x300 at 0, or (TitlesDataFormat at 0x3215 = 1, system 21.0.0 on) Deflate-compressed
    // there: a 16-bit size, then the compressed entries (up to 32 languages)
    static uint8_t t[32 * 0x300];
    const uint8_t *titles = b;
    int ntitles = 16;
    if (b[0x3215] == 1) {
        int csz = b[0] | b[1] << 8;
        if (csz <= 0 || csz > 0x2FFE) csz = 0x2FFE;
        memset(t, 0, sizeof t);
        int got = stbi_zlib_decode_noheader_buffer((char *)t, (int)sizeof t, (const char *)b + 2, csz);
        if (got < 0) got = stbi_zlib_decode_buffer((char *)t, (int)sizeof t, (const char *)b + 2, csz);
        if (got < 0x300) return 0;
        titles = t, ntitles = got / 0x300;
    }
    static const int order[] = {0, 1, 3, 4, 5, 6, 7, 8, 9, 10, 15, 11, 2, 12, 13, 14};
    for (int k = 0; k < 16; k++) {
        if (order[k] >= ntitles) continue;
        const char *name = (const char *)titles + order[k] * 0x300;
        if (!name[0] || !good_text(name, 0x200)) continue;
        snprintf(title, tn, "%.*s", 0x200, name);
        snprintf(publisher, pn, "%.*s", 0x100, good_text(name + 0x200, 0x100) ? name + 0x200 : "");
        snprintf(version, vn, "%.*s", 0x10, good_text((const char *)b + 0x3060, 0x10) ? (const char *)b + 0x3060 : "");
        return 1;
    }
    return 0;
}
// The program ID in a game's main.npdm (its ACI0 section), or 0.
uint64_t npdm_program_id(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[0x80];
    uint64_t id = 0;
    if (fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "META", 4)) {
        uint32_t off;
        memcpy(&off, h + 0x70, 4);
        uint8_t a[0x18];
        if (!fseek(f, off, SEEK_SET) && fread(a, 1, sizeof a, f) == sizeof a && !memcmp(a, "ACI0", 4)) memcpy(&id, a + 0x10, 8);
    }
    fclose(f);
    return id;
}

// settings nxlinux needs for particular games, unless the game's nxlinux.env sets them
static const struct { uint64_t id; const char *env; } g_defaults[] = {
    // Breath of the Wild: nxnative's registered cuts of main are its 60 fps changes, which need the frame step: at the
    // game's own 30 fps they are left out (the Frame rate option at 60 puts them in, with the frame step)
    {0x01007EF00011E000ull, "NXL_NO_CUTS=main"},
};
static void env_add(game_t *g, const char *kv) {
    const char *eq = strchr(kv, '=');
    if (!eq || eq == kv || g->nenv == 16) return;
    for (int i = 0; i < g->nenv; i++) if (!strncmp(g->env[i], kv, (size_t)(eq - kv) + 1)) return;   // already set
    snprintf(g->env[g->nenv++], sizeof g->env[0], "%s", kv);
}
// Reads a game's nxlinux.env (KEY=value lines) into the variables its run gets.
static void env_file(game_t *g) {
    char p[1100];
    snprintf(p, sizeof p, "%s/nxlinux.env", g->dir);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '#' || !*s) continue;
        s[strcspn(s, "\r\n")] = 0;
        env_add(g, s);
    }
    fclose(f);
}

// the builds nxnative's frame step knows (tools/gen_fstep.py's table, the one nxlinux's loader installs from)
extern const char *const nxl_fstep_builds[];
extern const size_t nxl_nfsteps;
static int fstep_known(const char *main_path) {   // the NSO's build ID (its header, 0x40) is one of them
    FILE *f = fopen(main_path, "rb");
    if (!f) return 0;
    unsigned char h[0x60];
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    if (n < sizeof h || memcmp(h, "NSO0", 4)) return 0;
    char bid[33];
    for (int i = 0; i < 16; i++) snprintf(bid + 2 * i, 3, "%02x", h[0x40 + i]);
    for (size_t i = 0; i < nxl_nfsteps; i++) if (!strcmp(nxl_fstep_builds[i], bid)) return 1;
    return 0;
}

// A game in a folder (ExeFS/main, maybe RomFS): its title, icon, background, program ID and graphics API; 0 when it is
// not one.
static int game_load(const char *dir, game_t *g) {
    memset(g, 0, sizeof *g);
    static const char *const exefs[] = {"ExeFS", "exefs", NULL}, *const romfs[] = {"RomFS", "romfs", NULL};
    if (!find_child(dir, exefs, 1, g->exefs, sizeof g->exefs)) return 0;
    char probe[1200];
    snprintf(probe, sizeof probe, "%s/main", g->exefs);
    if (!is_file(probe)) return 0;
    snprintf(g->dir, sizeof g->dir, "%s", dir);
    g->fstep = fstep_known(probe);
    g->gfx = gfx_api(g->exefs);
    find_child(dir, romfs, 1, g->romfs, sizeof g->romfs);
    static const char *const dlc[] = {"DLC", "dlc", "aoc", NULL};   // its DLC: DLC/<index>/, each one's RomFS extracted
    if (find_child(dir, dlc, 1, g->dlc, sizeof g->dlc)) {
        DIR *d = opendir(g->dlc);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char q[1300];
            snprintf(q, sizeof q, "%s/%s", g->dlc, e->d_name);
            if (e->d_name[0] >= '1' && e->d_name[0] <= '9' && strspn(e->d_name, "0123456789") == strlen(e->d_name) && is_dir(q)) g->ndlc++;
        }
        if (d) closedir(d);
        if (!g->ndlc) g->dlc[0] = 0;
    }
    snprintf(probe, sizeof probe, "%s/main.npdm", g->exefs);
    g->program_id = npdm_program_id(probe);
    // the title data: the game folder, or a control folder in it
    static const char *const ctl[] = {"control", "Control", "control.nca", NULL}, *const nacp[] = {"control.nacp", NULL};
    char cdir[1100], np[1200];
    const char *where[2] = {dir, NULL};
    if (find_child(dir, ctl, 1, cdir, sizeof cdir)) where[1] = cdir;
    for (int w = 0; w < 2 && where[w]; w++) {
        if (!g->title_from_nacp && find_child(where[w], nacp, 0, np, sizeof np))
            g->title_from_nacp = nacp_read(np, g->title, sizeof g->title, g->publisher, sizeof g->publisher, g->version, sizeof g->version);
        if (!g->icon[0]) {   // icon_AmericanEnglish.dat first, else any icon_*.dat
            static const char *const ae[] = {"icon_AmericanEnglish.dat", NULL};
            if (!find_child(where[w], ae, 0, g->icon, sizeof g->icon)) {
                DIR *d = opendir(where[w]);
                struct dirent *e;
                while (d && (e = readdir(d)))
                    if (!strncasecmp(e->d_name, "icon_", 5) && strlen(e->d_name) > 4 && !strcasecmp(e->d_name + strlen(e->d_name) - 4, ".dat")) {
                        snprintf(g->icon, sizeof g->icon, "%s/%s", where[w], e->d_name);
                        break;
                    }
                if (d) closedir(d);
            }
        }
    }
    static const char *const cover[] = {"cover.png", "cover.jpg", "cover.jpeg", "icon.png", "icon.jpg", NULL};
    if (!g->icon[0]) find_child(dir, cover, 0, g->icon, sizeof g->icon);
    static const char *const bg[] = {"background.png", "background.jpg", "background.jpeg", NULL};
    find_child(dir, bg, 0, g->background, sizeof g->background);
    if (!g->title[0]) {   // the folder's name, without a trailing version ("Game-1.2" stays as it is)
        const char *base = strrchr(dir, '/');
        snprintf(g->title, sizeof g->title, "%s", base ? base + 1 : dir);
    }
    env_file(g);
    for (size_t i = 0; i < sizeof g_defaults / sizeof *g_defaults; i++) if (g_defaults[i].id == g->program_id) env_add(g, g_defaults[i].env);
    return 1;
}

static int by_title(const void *a, const void *b) { return strcasecmp(((const game_t *)a)->title, ((const game_t *)b)->title); }
// Finds the games in a folder and the folders under it (two levels), up to 1000.
static void scan_dir(const char *dir, int depth, game_t **out, int *n, int *cap) {
    if (*n >= 1000) return;
    if (*n == *cap) *out = realloc(*out, (size_t)(*cap = *cap ? *cap * 2 : 16) * sizeof **out);
    if (game_load(dir, &(*out)[*n])) { (*n)++; return; }
    if (depth == 0) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    char **subs = NULL;
    int ns = 0;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!is_dir(p) || !strcasecmp(e->d_name, "Saves backup")) continue;   // (not games)
        subs = realloc(subs, (size_t)(ns + 1) * sizeof *subs);
        subs[ns++] = strdup(p);
    }
    closedir(d);
    for (int i = 0; i < ns; i++) { scan_dir(subs[i], depth - 1, out, n, cap); free(subs[i]); }
    free(subs);
}
char g_library_root[1024];

// The games in the games folder, sorted by title.
int library_scan(const char *root, game_t **out) {
    *out = NULL;
    int n = 0, cap = 0;
    snprintf(g_library_root, sizeof g_library_root, "%s", root ? root : "");
    if (root && *root) scan_dir(root, 2, out, &n, &cap);
    if (n) qsort(*out, (size_t)n, sizeof **out, by_title);
    return n;
}

// ---------- settings ----------
static void config_path(char *p, size_t n, int mkdirs) {
    char dir[900];
    nx_dir(0, "", dir, sizeof dir);
    if (mkdirs) nx_mkdirs(dir);
    snprintf(p, n, "%s/launcher.ini", dir);
}
// Reads NativeNX's settings (~/.config/nxlinux/launcher.ini), the defaults first.
void config_load(config_t *c) {
    memset(c, 0, sizeof *c);
    c->blur = 2, c->sounds = 1, c->volume = 60, c->sound_style = 0, c->sharpness = 2;
    char p[1000];
    config_path(p, sizeof p, 0);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        while (isspace((unsigned char)*k)) k++;
        for (char *e = k + strlen(k); e > k && isspace((unsigned char)e[-1]);) *--e = 0;
        while (isspace((unsigned char)*v)) v++;
        if (!strcmp(k, "games")) snprintf(c->games, sizeof c->games, "%s", v);
        else if (!strcmp(k, "background")) snprintf(c->background, sizeof c->background, "%s", v);
        else if (!strcmp(k, "fps_counter")) c->fps_counter = atoi(v) != 0;
        else if (!strcmp(k, "game_fullscreen")) c->game_fullscreen = atoi(v) != 0;
        else if (!strcmp(k, "upscale")) c->upscale = atoi(v) >= 0 && atoi(v) <= 2 ? atoi(v) : 0;
        else if (!strcmp(k, "sharpness")) c->sharpness = atoi(v) >= 0 && atoi(v) <= 3 ? atoi(v) : 2;
        else if (!strcmp(k, "blur")) c->blur = atoi(v) < 0 ? 0 : atoi(v) > 3 ? 3 : atoi(v);
        else if (!strcmp(k, "sounds")) c->sounds = atoi(v) != 0;
        else if (!strcmp(k, "sound_style")) c->sound_style = atoi(v) < 0 || atoi(v) > 2 ? 0 : atoi(v);
        else if (!strcmp(k, "volume")) c->volume = atoi(v) < 0 ? 0 : atoi(v) > 100 ? 100 : atoi(v);
    }
    fclose(f);
}
// Writes NativeNX's settings.
void config_save(const config_t *c) {
    char p[1000];
    config_path(p, sizeof p, 1);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "[launcher] cannot write %s: %s\n", p, strerror(errno)); return; }
    fprintf(f, "# nxlinux launcher settings\ngames = %s\nbackground = %s\nblur = %d\nsounds = %d\nvolume = %d\nsound_style = %d\nfps_counter = %d\n"
               "game_fullscreen = %d\nupscale = %d\nsharpness = %d\n", c->games, c->background, c->blur, c->sounds, c->volume, c->sound_style, c->fps_counter,
               c->game_fullscreen, c->upscale, c->sharpness);
    fclose(f);
}

// ---------- mods and a game's options ----------
static int by_mod_name(const void *a, const void *b) { return strcasecmp(((const mod_t *)a)->name, ((const mod_t *)b)->name); }
// the romfs of a mod folder: romfs / RomFS in it, or under [atmosphere/]contents/<any>/
static int mod_romfs(const char *m, char *out, size_t n, char *exefs) {   // exefs: 1100 bytes, the first exefs folder found
    static const char *const rn[] = {"romfs", "RomFS", NULL}, *const en[] = {"exefs", "ExeFS", "exefs_patches", NULL}, *const cn[] = {"contents", "Contents", NULL}, *const an[] = {"atmosphere", "Atmosphere", NULL};
    char t[1100];
    if (!exefs[0] && find_child(m, en, 1, t, sizeof t)) snprintf(exefs, 1100, "%s", t);
    if (find_child(m, rn, 1, out, n)) return 1;
    char base[1100], c[1100];
    snprintf(base, sizeof base, "%s", m);
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) { if (!find_child(m, an, 1, base, sizeof base)) break; }
        if (!find_child(base, cn, 1, c, sizeof c)) continue;
        DIR *d = opendir(c);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char tid[1200];
            snprintf(tid, sizeof tid, "%s/%s", c, e->d_name);
            if (!exefs[0] && find_child(tid, en, 1, t, sizeof t)) snprintf(exefs, 1100, "%s", t);
            if (find_child(tid, rn, 1, out, n)) { closedir(d); return 1; }
        }
        if (d) closedir(d);
    }
    return 0;
}
// a mod unpacked with folders around it ("Linkle Mod/romfs", "Mod v2/01007EF00011E000/romfs"): the first romfs found up
// to depth folders down
static int mod_find_deep(const char *dir, int depth, char *out, size_t n, char *exefs) {
    if (depth <= 0) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int found = 0;
    while (!found && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (!is_dir(p)) continue;
        if ((!strcasecmp(e->d_name, "exefs") || !strcasecmp(e->d_name, "exefs_patches")) && !exefs[0]) snprintf(exefs, 1100, "%s", p);
        if (!strcasecmp(e->d_name, "romfs")) { snprintf(out, n, "%s", p); found = 1; break; }
        found = mod_find_deep(p, depth - 1, out, n, exefs);
    }
    closedir(d);
    return found;
}
// A game's mods: each folder in its mods folder (a romfs, an exefs patch, or a BCML mod), sorted by name.
int mods_list(const game_t *g, mod_t *out, int max) {
    char dir[1100];
    static const char *const mn[] = {"mods", "Mods", NULL};
    if (!find_child(g->dir, mn, 1, dir, sizeof dir)) return 0;
    DIR *d = opendir(dir);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char m[1100];
        snprintf(m, sizeof m, "%s/%s", dir, e->d_name);
        if (!is_dir(m)) continue;
        mod_t *o = &out[n];
        memset(o, 0, sizeof *o);
        snprintf(o->name, sizeof o->name, "%s", e->d_name);
        if (bnp_detect(m, &o->b)) { o->bnp = 1; snprintf(o->romfs, sizeof o->romfs, "%s", o->b.romfs[0] ? o->b.romfs : o->b.aoc); n++; continue; }   // (a mod of DLC files only: those)   // a BCML mod (its options' folders are its own)
        if (!mod_romfs(m, o->romfs, sizeof o->romfs, o->exefs)) mod_find_deep(m, 4, o->romfs, sizeof o->romfs, o->exefs);
        o->has_exefs = o->exefs[0] != 0;
        n++;
    }
    closedir(d);
    qsort(out, (size_t)n, sizeof *out, by_mod_name);
    return n;
}
// Where a game's options are kept (~/.config/nxlinux/games/<folder name>.ini).
static void gopts_path(const game_t *g, char *p, size_t n, int mk) {
    char dir[900];
    nx_dir(0, "games", dir, sizeof dir);
    if (mk) nx_mkdirs(dir);
    const char *base = strrchr(g->dir, '/') ? strrchr(g->dir, '/') + 1 : g->dir;
    char safe[256];
    snprintf(safe, sizeof safe, "%s", base);
    for (char *c = safe; *c; c++) if (*c == '/' || *c == ' ') *c = '_';
    snprintf(p, n, "%s/%s.ini", dir, safe);
}
// Reads a game's options (mode, graphics, mods on, BCML options).
void gopts_load(const game_t *g, gopts_t *o) {
    memset(o, 0, sizeof *o);
    o->native = 1;
    char p[1200];
    gopts_path(g, p, sizeof p, 0);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "native = ", 9)) o->native = atoi(line + 9) != 0;
        else if (!strncmp(line, "aniso = ", 8)) o->aniso = atoi(line + 8);
        else if (!strncmp(line, "fps = ", 6)) o->fps = atoi(line + 6) == 60 ? 60 : 0;
        else if (!strncmp(line, "fill = ", 7)) o->fill = atoi(line + 7) != 0;
        else if (!strncmp(line, "res = ", 6)) { int r = atoi(line + 6); o->res = r == 15 || r == 20 || r == 25 || r == 30 || r == 40 ? r : 0; }
        else if (!strncmp(line, "mod = ", 6) && o->non < 32) snprintf(o->on[o->non++], sizeof o->on[0], "%s", line + 6);
        else if (!strncmp(line, "bnpopt = ", 9) && o->nbnpopt < 64) snprintf(o->bnpopt[o->nbnpopt++], sizeof o->bnpopt[0], "%s", line + 9);
    }
    fclose(f);
}
// Writes a game's options.
void gopts_save(const game_t *g, const gopts_t *o) {
    char p[1200];
    gopts_path(g, p, sizeof p, 1);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "[launcher] cannot write %s: %s\n", p, strerror(errno)); return; }
    fprintf(f, "# nxlinux launcher: the options of %s\nnative = %d\naniso = %d\nfps = %d\nres = %d\nfill = %d\n", g->title, o->native, o->aniso, o->fps, o->res, o->fill);
    for (int i = 0; i < o->non; i++) fprintf(f, "mod = %s\n", o->on[i]);
    for (int i = 0; i < o->nbnpopt; i++) fprintf(f, "bnpopt = %s\n", o->bnpopt[i]);
    fclose(f);
}
static int bnp_key(const mod_t *m, int opt, char *k, size_t n) { return snprintf(k, n, "%s|%s|", m->name, m->b.opt[opt].folder); }
int gopts_bnp_on(const gopts_t *o, const mod_t *m, int opt) {
    char k[400];
    int kl = bnp_key(m, opt, k, sizeof k);
    for (int i = 0; i < o->nbnpopt; i++) if (!strncmp(o->bnpopt[i], k, (size_t)kl)) return atoi(o->bnpopt[i] + kl) != 0;
    return m->b.opt[opt].def;
}
// Turns a BCML mod's option on or off in a game's options.
void gopts_bnp_set(gopts_t *o, const mod_t *m, int opt, int on) {
    char k[400];
    int kl = bnp_key(m, opt, k, sizeof k);
    int i = 0;
    while (i < o->nbnpopt && strncmp(o->bnpopt[i], k, (size_t)kl)) i++;
    if (i == o->nbnpopt) { if (o->nbnpopt == 64) return; o->nbnpopt++; }
    snprintf(o->bnpopt[i], sizeof o->bnpopt[0], "%s%d", k, on);
}
// ---------- save backups ----------
void save_backup_dir(const game_t *g, char *out, size_t n) {
    char t[200];
    size_t k = 0;
    for (const char *c = g->title; *c && k < sizeof t - 1; c++) t[k++] = *c == '/' || *c == ':' || *c == '\\' ? '-' : *c;
    t[k] = 0;
    snprintf(out, n, "%s/Saves backup/%s", g_library_root[0] ? g_library_root : ".", t);
}
static int copy_tree(const char *from, const char *to, int depth) {   // a folder and what is in it; 0 done
    if (depth > 16 || (mkdir(to, 0755) && errno != EEXIST)) return -1;
    DIR *d = opendir(from);
    if (!d) return -1;
    struct dirent *e;
    int r = 0;
    while (!r && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[2048], b[2048];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name), snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        if (is_dir(a)) { r = copy_tree(a, b, depth + 1); continue; }
        FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL;
        char buf[65536];
        size_t n;
        while (in && out && (n = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, n, out) != n) r = -1;
        if (!in || !out) r = -1;
        if (in) fclose(in);
        if (out && fclose(out)) r = -1;
    }
    closedir(d);
    return r;
}
// Deletes a folder and everything in it (not following links, up to 32 levels).
static int remove_tree(const char *p, int depth) {
    DIR *d = opendir(p);
    if (!d) return unlink(p);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[2048];
        snprintf(a, sizeof a, "%s/%s", p, e->d_name);
        if (is_dir(a) && depth < 16) remove_tree(a, depth + 1); else unlink(a);
    }
    closedir(d);
    return rmdir(p);
}
// Whether a folder has anything in it.
static int has_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int any = 0;
    while (!any && (e = readdir(d))) any = strcmp(e->d_name, ".") && strcmp(e->d_name, "..");
    closedir(d);
    return any;
}
// Copies a game's save folder into "<games folder>/Saves backup/<title>/<date time> <label>/"; 1 when there was nothing
// to copy.
int save_backup(const game_t *g, const char *save_dir, const char *label, char *made, size_t mn) {
    if (!has_files(save_dir)) return 1;
    char root[1200], dir[1500], ts[64];
    save_backup_dir(g, root, sizeof root);
    nx_mkdirs(root);
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof ts, "%Y-%m-%d %H-%M-%S", &tm);
    snprintf(dir, sizeof dir, "%s/%s%s%s", root, ts, label && *label ? " " : "", label ? label : "");
    for (int k = 2; is_dir(dir) && k < 100; k++) snprintf(dir, sizeof dir, "%s/%s %s (%d)", root, ts, label ? label : "", k);
    if (copy_tree(save_dir, dir, 0)) return -1;
    if (made) snprintf(made, mn, "%s", dir);
    return 0;
}
static int by_name_desc(const void *a, const void *b) { return strcmp((const char *)b, (const char *)a); }
// The backups of a game's save, newest first.
int save_backups(const game_t *g, char names[][128], int max) {
    char root[1200];
    save_backup_dir(g, root, sizeof root);
    DIR *d = opendir(root);
    struct dirent *e;
    int n = 0;
    while (d && (e = readdir(d)) && n < max) {
        char p[1500];
        snprintf(p, sizeof p, "%s/%s", root, e->d_name);
        if (e->d_name[0] != '.' && is_dir(p)) snprintf(names[n++], 128, "%s", e->d_name);
    }
    if (d) closedir(d);
    qsort(names, (size_t)n, 128, by_name_desc);   // the date first in each name: newest first
    return n;
}
// Puts a backup back as the game's save (the save before it kept aside until the copy is done).
int save_restore(const game_t *g, const char *save_dir, const char *name) {
    char root[1200], from[1500];
    save_backup_dir(g, root, sizeof root);
    snprintf(from, sizeof from, "%s/%s", root, name);
    if (!is_dir(from) || strchr(name, '/')) return -1;
    char old[1300];
    snprintf(old, sizeof old, "%s.restoring", save_dir);
    remove_tree(old, 0);
    if (is_dir(save_dir) && rename(save_dir, old)) return -1;
    if (copy_tree(from, save_dir, 0)) { remove_tree(save_dir, 0); rename(old, save_dir); return -1; }
    remove_tree(old, 0);
    return 0;
}
int gopts_mod_on(const gopts_t *o, const char *name) { for (int i = 0; i < o->non; i++) if (!strcmp(o->on[i], name)) return 1; return 0; }
void gopts_mod_set(gopts_t *o, const char *name, int on) {
    for (int i = 0; i < o->non; i++) if (!strcmp(o->on[i], name)) { if (!on) memmove(o->on[i], o->on[i + 1], (size_t)(--o->non - i) * sizeof o->on[0]); return; }
    if (on && o->non < 32) snprintf(o->on[o->non++], sizeof o->on[0], "%s", name);
}
