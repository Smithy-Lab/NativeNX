// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, starting a game: the loading screen, BCML mods merged first (bnp.c), nxlinux started and watched (nativenx.h lists the parts).
#include "nativenx.h"

// ---------- launching ----------
nxl_proc g_child;   // the game running (src/platform.h), 0 for none
int g_last_status;
char g_log[1200], g_toast[600];
uint64_t g_toast_until;
void toast(const char *m) { snprintf(g_toast, sizeof g_toast, "%s", m); g_toast_until = SDL_GetTicks() + 6000; sound(SND_ERROR); }
void open_folder(const char *dir) { nxl_open_folder(dir); }   // in the desktop's file manager (src/platform_*.c)
void mkdirs(const char *p) { nx_mkdirs(p); }
int g_loading, g_load_gi, g_load_stage;
int g_play_gi = -1; uint64_t g_play_t0;   // the game played (from a launch in the window) and when its first frame came
uint64_t g_play_acc;   // ms played before the last pause (the game waiting at NativeNX's Home)
int g_suspended;       // the game waits at NativeNX's Home: stopped (SIGSTOP), its window minimised
int game_waiting(int gi) { return g_suspended && gi == g_play_gi && gi >= 0; }
// The running game (and the one loading) is known by its place in g_games, which reorders (the favourites filter, a
// rescan, a game played going to the front): kept by folder across that
void play_dirs_keep(char pd[1024], char ld[1024]) {
    snprintf(pd, 1024, "%s", g_play_gi >= 0 && g_play_gi < g_ngames ? g_games[g_play_gi].dir : "");
    snprintf(ld, 1024, "%s", g_load_gi >= 0 && g_load_gi < g_ngames ? g_games[g_load_gi].dir : "");
}
void play_dirs_follow(const char *pd, const char *ld) {
    int p = -1, l = -1;
    for (int i = 0; i < g_ngames; i++) { if (pd[0] && !strcmp(g_games[i].dir, pd)) p = i; if (ld[0] && !strcmp(g_games[i].dir, ld)) l = i; }
    if (g_play_gi >= 0) g_play_gi = p;   // (-1: its folder is gone: nothing more is kept for it)
    if (g_load_gi >= 0) g_load_gi = l;
}
uint64_t play_ms(void) { return g_play_acc + (g_play_t0 ? SDL_GetTicks() - g_play_t0 : 0); }   // this session
uint64_t g_load_t0;
static long g_load_off;
char g_load_last[200];
static const char *const LOAD_STAGE[] = {"Starting nxlinux", "Loading the game's code", "Translating the game's code for this PC", "Starting the game", "Showing the first frame"};
// Follows the starting game's log, to show its loading stage (code loaded, translated, first frame).
void loading_poll(void) {
    if (!g_loading) return;
    FILE *f = fopen(g_log, "r");
    if (!f) return;
    if (fseek(f, g_load_off, SEEK_SET)) { fclose(f); return; }
    char line[1024];
    int done = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        if (n && line[n - 1] != '\n' && !feof(f)) continue;   // a partial line: read again later
        g_load_off = ftell(f);
        line[strcspn(line, "\r\n")] = 0;
        if (strstr(line, "[nxl] loaded ")) { if (g_load_stage < 1) g_load_stage = 1; }
        else if (strstr(line, "entry points")) { if (g_load_stage < 2) g_load_stage = 2; }
        else if (strstr(line, "recompiled ")) { if (g_load_stage < 3) g_load_stage = 3; }
        else if (strstr(line, "first frame presented")) { if (g_load_stage < 4) g_load_stage = 4; }
        if (strstr(line, "first frame shown in the window")) done = 1;
        if (line[0]) {   // the latest line, without nxlinux's prefixes
            const char *t = line;
            while (*t == '[') { const char *e = strchr(t, ']'); if (!e) break; t = e + 1; while (*t == ' ') t++; }
            snprintf(g_load_last, sizeof g_load_last, "%s", t);
        }
    }
    fclose(f);
    if (done) {   // the game's window shows the game: the launcher steps aside
        g_loading = 0;
        g_play_t0 = SDL_GetTicks();
        LOG("the game's first frame is on screen after %.1f s", (SDL_GetTicks() - g_load_t0) / 1000.0);
        if (g_win) SDL_HideWindow(g_win);
    }
}
// Draws the loading screen: the game's picture, its title and the stage.
void draw_loading(void) {
    draw_background();
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 10, 12, 18, 150);
    SDL_RenderFillRect(g_r, &all);
    const float sz = 260, x = 120, y = (g_vh - sz) / 2 - 20;
    if (g_load_gi >= 0 && g_load_gi < g_ngames) draw_tile(g_load_gi, x, y, sz, 0, 0);
    float tx = x + sz + 60, ty = y + 10;
    const char *title = g_load_gi >= 0 && g_load_gi < g_ngames ? g_games[g_load_gi].title : "";
    text(F_TITLE, tx, ty, C_TEXT, title);
    ty += 70;
    float secs = (SDL_GetTicks() - g_load_t0) / 1000.f;
    // a spinner: a ring of dots, one bright, turning
    for (int i = 0; i < 12; i++) {
        float a = (float)i / 12 * 6.2831853f, ph = fmodf(secs * 1.5f, 1.f) * 12, d = fmodf(ph - i + 12, 12);
        float cx = tx + 18 + 14 * sinf(a), cy = ty + 18 - 14 * cosf(a);
        uint8_t al = (uint8_t)(255 * (d < 1 ? 1 : 0.85f / d + 0.1f));
        rrect(cx - 3, cy - 3, 6, 6, 3, (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, al});
    }
    char st[160];
    snprintf(st, sizeof st, "%s...", LOAD_STAGE[g_load_stage]);
    text(F_BODY, tx + 52, ty + 4, C_TEXT, st);
    char tm[64];
    snprintf(tm, sizeof tm, "%d:%02d", (int)secs / 60, (int)secs % 60);
    text(F_BODY, tx + 52 + text_w(F_BODY, st) + 24, ty + 4, C_DIM, tm);
    ty += 56;
    for (int i = 0; i < 5; i++) {   // the steps: done, now, to come
        rgba c = i < g_load_stage ? C_DIM : i == g_load_stage ? C_TEXT : (rgba){C_DIM.r, C_DIM.g, C_DIM.b, 90};
        rgba dc = i < g_load_stage ? C_ACCENT : i == g_load_stage ? C_TEXT : (rgba){255, 255, 255, 50};   // done, now, to come
        rrect(tx + 10, ty + 8, 10, 10, 5, dc);
        text(F_SMALL, tx + 36, ty, c, LOAD_STAGE[i]);
        ty += 30;
    }
    if (g_load_stage == 2 && secs > 15)
        text(F_SMALL, tx, ty + 14, C_ACCENT, "The first start of a game, or of new settings for it, takes a few minutes here; later starts take seconds.");
    if (g_load_last[0]) {   // what nxlinux wrote last: it changes while it works
        char l[120];
        snprintf(l, sizeof l, "%.110s", g_load_last);
        text(F_SMALL, 120, g_vh - 120, (rgba){C_DIM.r, C_DIM.g, C_DIM.b, 160}, l);
    }
    hint(g_vw - 230, g_vh - 70, btn("back"), "Stop", C_TEXT);
}
int g_ach_fd = -1;   // the pipe to the running game's window: its achievement pop-ups and the menu (NXL_NOTIFY_FD)
int g_game_fd = -1;  // ... and from it: the Home button, the buttons while the menu is up (NXL_LAUNCHER_FD)
char g_pending_launch[1024];   // a game (its folder) to start once the one closing has ended (it waited at Home)
// Reads a file of the game's own (from its RomFS folder), for the BCML merge.
static int stock_read(void *ctx, const char *path, uint8_t **data, size_t *n) {
    stock_t *st = ctx;
    char p[2200];
    snprintf(p, sizeof p, "%s/%s", st->romfs, path);
    FILE *f = fopen(p, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = sz >= 0 ? malloc((size_t)sz + 1) : NULL;
    int ok = b && fread(b, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (!ok) { free(b); return -1; }
    *data = b, *n = (size_t)sz;
    return 0;
}
static int dir_exists(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
// Reads a file of the game's DLC (the first DLC folder that has it), for the BCML merge.
static int stock_aoc_read(void *ctx, const char *path, uint8_t **data, size_t *n) {
    stock_aoc_t *a = ctx;
    for (int i = 0; i < a->n; i++) {
        stock_t st = {a->dir[i]};
        if (!stock_read(&st, path, data, n)) return 0;
    }
    return -1;
}
// The game's DLC folders (DLC/<index>/), lowest index first
static void dlc_dirs(const game_t *g, stock_aoc_t *a) {
    a->n = 0;
    if (!g->dlc[0]) return;
    for (int i = 1; i < 2000 && a->n < 32; i++) {
        snprintf(a->dir[a->n], sizeof a->dir[0], "%s/%d", g->dlc, i);
        if (dir_exists(a->dir[a->n])) a->n++;
    }
}
static uint64_t fnv(uint64_t h, const void *p, size_t n) { for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull; return h; }
static uint64_t fnv_file(uint64_t h, const char *path) {   // a file's name, size and time
    struct stat st;
    h = fnv(h, path, strlen(path));
    if (!stat(path, &st)) h = fnv(h, &st.st_size, sizeof st.st_size), h = fnv(h, &st.st_mtime, sizeof st.st_mtime);
    return h;
}
// Merges the BCML mods turned on (once, kept in the cache); the folders to lay over the game's files and the DLC's.
static int bnp_prepare(const game_t *g, const gopts_t *o, mod_t *mods, int nm, char *out, size_t on, char *out_aoc, size_t aon) {
    out[0] = 0, out_aoc[0] = 0;
    bnp_t b[40];
    uint32_t mask[40];
    int nb = 0;
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, "bnp4", 4);   // the merge's version: a new one remakes every merge made before (bnp2: AAMP logs; bnp3: sizes as BCML; bnp4: DLC files)
    for (int i = nm - 1; i >= 0; i--) {   // lowest priority first: the last in the list (the first wins, as for other mods)
        if (!mods[i].bnp || !gopts_mod_on(o, mods[i].name) || !mods[i].romfs[0]) continue;
        b[nb] = mods[i].b, mask[nb] = 0;
        for (int k = 0; k < mods[i].b.nopt; k++) if (gopts_bnp_on(o, &mods[i], k)) mask[nb] |= 1u << k;
        char p[1300];
        snprintf(p, sizeof p, "%s/info.json", b[nb].dir), h = fnv_file(h, p);
        snprintf(p, sizeof p, "%s/logs/rstb.json", b[nb].dir), h = fnv_file(h, p);
        snprintf(p, sizeof p, "%s/logs/packs.json", b[nb].dir), h = fnv_file(h, p);
        snprintf(p, sizeof p, "%s/logs/deepmerge.aamp", b[nb].dir), h = fnv_file(h, p);
        snprintf(p, sizeof p, "%s/logs/aslist.aamp", b[nb].dir), h = fnv_file(h, p);
        h = fnv(h, &mask[nb], sizeof mask[nb]);
        nb++;
    }
    if (!nb) return 0;
    h = fnv_file(h, g->romfs);
    stock_aoc_t sa;   // the DLC: their files under the mods' DLC files
    dlc_dirs(g, &sa);
    for (int i = 0; i < sa.n; i++) h = fnv_file(h, sa.dir[i]);
    char sub[200];
    snprintf(sub, sizeof sub, "bnp/%016llx/%016llx", (unsigned long long)g->program_id, (unsigned long long)h);
    char dir[1100], done[1200];
    nx_cache_dir(sub, dir, sizeof dir);
    snprintf(done, sizeof done, "%s/made", dir);
    snprintf(out, on, "%s/romfs", dir);
    char aocd[1200];
    snprintf(aocd, sizeof aocd, "%s/aoc", dir);
    if (!access(done, F_OK)) { LOG("BCML mods: the merge made before is used (%s)", dir); if (dir_exists(aocd)) snprintf(out_aoc, aon, "%s", aocd); return 0; }
    if (!g->romfs[0]) { toast("BCML mods need the game's RomFS"); return -1; }
    stock_t st = {g->romfs};
    char err[300] = "";
    LOG("BCML mods: merging %d with the game's files into %s", nb, dir);
    int r = bnp_build(b, mask, nb, stock_read, &st, sa.n ? stock_aoc_read : NULL, &sa, dir, err, sizeof err);
    if (!r && dir_exists(aocd)) snprintf(out_aoc, aon, "%s", aocd);
    if (r) { char m[400]; snprintf(m, sizeof m, "BCML mods could not be merged: %s", err); toast(m); LOG("%s", m); return -1; }
    FILE *f = fopen(done, "w");
    if (f) fclose(f);
    return 0;
}
// Starts a game: its options and mods into nxlinux's environment, the save chosen, then nxlinux run and watched.
void launch(const game_t *g) {
    if (g_child) return;
    char nx[1100];
    if (nxl_exe_dir(nx, sizeof nx)) nx[0] = 0;
    strncat(nx, "/nxlinux" NXL_EXE_SUFFIX, sizeof nx - strlen(nx) - 1);
    if (getenv("NXL_LAUNCHER_NXLINUX")) snprintf(nx, sizeof nx, "%s", getenv("NXL_LAUNCHER_NXLINUX"));   // another nxlinux (tests)
    if (access(nx, X_OK)) { char m[1300]; snprintf(m, sizeof m, "nxlinux was not found next to the launcher (%s)", nx); toast(m); return; }
    char logs[1000];
    nx_dir(1, "logs", logs, sizeof logs);
    mkdirs(logs);
    char base[200];
    snprintf(base, sizeof base, "%s", strrchr(g->dir, '/') ? strrchr(g->dir, '/') + 1 : g->dir);
    for (char *c = base; *c; c++) if (*c == '/' || *c == ' ') *c = '_';
    snprintf(g_log, sizeof g_log, "%s/%s.log", logs, base);
    char save[1100], temp[1100];
    profile_save_dir(&g_profs, g_profs.cur, g, save, sizeof save);   // the profile's own saves (profile.c)
    snprintf(temp, sizeof temp, "%s/temp", g->dir);
    mkdirs(save), mkdirs(temp);
    char album[1100];   // F12 in the game saves a screenshot here (nxlinux's NXL_ALBUM_DIR; the Album shows them)
    { char sub[300]; snprintf(sub, sizeof sub, "album/%s", base); nx_dir(1, sub, album, sizeof album); }
    // the game's options (Options): native, or with the changes chosen there
    gopts_t o;
    gopts_load(g, &o);
    static mod_t mods[40];
    int nm = o.native ? 0 : mods_list(g, mods, 40);
    char overlay[4096] = "", aniso[16] = "", patches[4096] = "";
    for (int i = 0; i < nm; i++) {   // the mods turned on: their code patches' folders
        if (!gopts_mod_on(&o, mods[i].name) || !mods[i].exefs[0]) continue;
        if (strlen(patches) + strlen(mods[i].exefs) + 2 >= sizeof patches) break;
        if (patches[0]) strcat(patches, NXL_LIST_SEP);
        strcat(patches, mods[i].exefs);
    }
    char bnp_romfs[1200] = "", bnp_aoc[1200] = "";   // the BCML mods turned on: merged with the game's files into one folder (bnp.c), in the list where the first is
    if (bnp_prepare(g, &o, mods, nm, bnp_romfs, sizeof bnp_romfs, bnp_aoc, sizeof bnp_aoc)) return;
    int bnp_put = 0;
    for (int i = 0; i < nm; i++) {
        if (!gopts_mod_on(&o, mods[i].name) || !mods[i].romfs[0]) continue;
        const char *rf = mods[i].romfs;
        if (mods[i].bnp) { if (bnp_put++ || !bnp_romfs[0]) continue; rf = bnp_romfs; }
        if (strlen(overlay) + strlen(rf) + 2 >= sizeof overlay) { LOG("too many mods for one list: %s and after are left out", mods[i].name); break; }
        if (overlay[0]) strcat(overlay, NXL_LIST_SEP);
        strcat(overlay, rf);
    }
    if (!o.native && o.aniso > 1) snprintf(aniso, sizeof aniso, "%d", o.aniso);
    int fps60 = !o.native && o.fps == 60 && g->fstep;
    char res[8] = "";
    if (!o.native && o.res) snprintf(res, sizeof res, "%d.%d", o.res / 10, o.res % 10);
    LOG("starting %s (%s), log %s; %s%s%s%s%s", g->title, g->dir, g_log, o.native ? "native" : "with changes", aniso[0] ? ", anisotropic filtering " : "", aniso, overlay[0] ? ", mods" : "", fps60 ? ", up to 60 fps" : "");
    // nxlinux's environment: what NativeNX chose ("NAME=value"; "NAME" alone: removed), then nxlinux started with its
    // output in the log, and two pipes when there is a window: the game's window reads its pop-ups and the menu from
    // the first (NXL_NOTIFY_FD, gfx/vk_overlay.c) and asks for the menu down the second (NXL_LAUNCHER_FD)
    enum { NENV = 96 };
    static char envs[NENV][4300];
    const char *ev[NENV + 1];
    int ne = 0;
#define ENV(...) do { if (ne < NENV) { snprintf(envs[ne], sizeof envs[0], __VA_ARGS__); ev[ne] = envs[ne]; ne++; } } while (0)
    if (g->romfs[0]) ENV("DM_ROMFS=%s", g->romfs);
    if (g->dlc[0]) ENV("DM_AOC_DIR=%s", g->dlc); else ENV("DM_AOC_DIR");   // its DLC: DLC/<index>/ (nxnative's hle_fs.c)
    ENV("DM_SAVE=%s", save);
    ENV("DM_TEMP=%s", temp);
    for (int i = 0; i < g->nenv; i++) ENV("%s", g->env[i]);   // the game's own (its game.tsv entry, library.c)
    if (overlay[0]) ENV("DM_ROMFS_OVERLAY=%s", overlay);   // the mods' files first, then the game's (nxnative's hle_fs.c)
    if (bnp_aoc[0]) ENV("DM_AOC_OVERLAY=%s", bnp_aoc); else ENV("DM_AOC_OVERLAY");   // ... and over the DLC's (compat/nxnative/patches/hle_fs.py)
    if (patches[0]) ENV("NXL_EXEFS_PATCHES=%s", patches);   // the mods' code patches (src/exefs_patch.c)
    ENV("NXL_ALBUM_DIR=%s", album);   // F12's screenshots
    if (!o.native && o.fill) ENV("NXL_FILL=1");   // the picture fills the window (gfx/vk_present.c)
    if (aniso[0]) ENV("NXL_ANISO=%s", aniso);
    if (res[0]) ENV("NXL_RES_SCALE=%s", res);   // the resolution scale (gfx/vk_tex.c)
    if (g_cfg.fps_counter) ENV("NXL_FPS=1");   // the frame rate counter (gfx/vk_present.c)
    if (g_cfg.game_fullscreen) ENV("NXL_FULLSCREEN=1");
    static const char *const up_k[3] = {"fsr", "linear", "nearest"}, *const sh_k[4] = {"off", "1", "0.25", "0"};   // gfx/vk_upscale.c
    ENV("NXL_UPSCALE=%s", up_k[g_cfg.upscale % 3]);
    ENV("NXL_SHARPNESS=%s", sh_k[g_cfg.sharpness & 3]);
    if (fps60) {   // nxnative's frame step (loader.c install_fstep), which needs the build's cuts in
        ENV("DM_FPS_TARGET=60");
        ENV("NXL_NO_CUTS=");
    }
#undef ENV
    ev[ne] = NULL;
    const char *argv[] = {nx, "-v", g->exefs, NULL};
    nxl_spawn_t sp = {.exe = nx, .argv = argv, .log = g_log, .env = ev,
                      .to_env = g_win ? "NXL_NOTIFY_FD" : NULL, .from_env = g_win ? "NXL_LAUNCHER_FD" : NULL};
    nxl_proc pid = nxl_spawn(&sp);
    if (!pid) { toast("could not start the game"); return; }
    g_child = pid;
    g_ach_fd = sp.to_fd, g_game_fd = sp.from_fd;
    // the loading screen until the game's first frame is on screen (loading_poll); without a window, nothing to show
    if (g_win) g_loading = 1, g_load_t0 = SDL_GetTicks(), g_load_gi = (int)(g - g_games), g_load_off = 0, g_load_stage = 0, g_load_last[0] = 0;
    if (g_win) {   // played now: last played is now; the time counts from its first frame (loading_poll) to its end (child_poll)
        g_play_gi = (int)(g - g_games), g_play_t0 = 0, g_play_acc = 0;
        plays_add_time(&g_plays, g, 0, (long long)time(NULL));
        plays_save(g_profs.p[g_profs.cur].id, &g_plays);
        ach_check(g_play_gi, 1);   // started: the first achievement may be due (its pop-up waits for the game's window)
    }
}
// Checks whether the game has ended: its play time saved, achievements checked, the window back.
void child_poll(void) {
    if (!g_child) return;
    int st;
    if (!nxl_proc_ended(g_child, &st)) return;
    g_child = 0;
    g_loading = 0;
    g_last_status = st;
    if (g_ach_fd >= 0) close(g_ach_fd), g_ach_fd = -1;
    if (g_game_fd >= 0) close(g_game_fd), g_game_fd = -1;
    ov_game_ended();
    if (g_win) SDL_ShowWindow(g_win), SDL_RaiseWindow(g_win);
    if (g_play_gi >= 0 && g_play_gi < g_ngames) {   // its time, and the game to the front of the row
        long long secs = (long long)(play_ms() / 1000);
        plays_add_time(&g_plays, &g_games[g_play_gi], secs, (long long)time(NULL));
        plays_save(g_profs.p[g_profs.cur].id, &g_plays);
        LOG("played %s for %lld s", g_games[g_play_gi].title, secs);
        ach_check(g_play_gi, 1);   // its last save, its time: shown here, the game is gone
        order_games(g_play_gi);
    }
    g_play_gi = -1, g_play_t0 = 0, g_play_acc = 0;
    if (g_pending_launch[0]) {   // a game chosen while this one waited at Home: now it starts
        char d[1024];
        snprintf(d, sizeof d, "%s", g_pending_launch);
        g_pending_launch[0] = 0;
        for (int i = 0; i < g_ngames; i++) if (!strcmp(g_games[i].dir, d)) { g_sel = i; launch(&g_games[i]); return; }
    }
    if (st == 0) return;
    char m[1400];
    if (st < 0) snprintf(m, sizeof m, "The game stopped unexpectedly (signal %d). Its log: %s", -st, g_log);
    else snprintf(m, sizeof m, "The game ended with an error (%d). Its log: %s", st, g_log);
    toast(m);
}
