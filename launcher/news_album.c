// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, What's new and the Album (screenshots) (nativenx.h lists the parts).
#include "nativenx.h"

// ---------- What's new ----------
// launcher/data/whatsnew.md: "## " a heading, "- " a point, anything else a line
static char *g_news;
// Reads What's new (launcher/data/whatsnew.md) once.
static void news_load(void) {
    if (g_news) return;
    char p[1100];
    snprintf(p, sizeof p, "%s/whatsnew.md", g_data);
    FILE *f = fopen(p, "r");
    if (!f) { g_news = strdup("## What's new\nThe list of changes (whatsnew.md) is not there."); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    if (n < 0) n = 0;
    fseek(f, 0, SEEK_SET);
    g_news = calloc(1, (size_t)n + 1);
    if (fread(g_news, 1, (size_t)n, f) != (size_t)n) g_news[0] = 0;
    fclose(f);
}
// text wrapped to width w from (x, y): returns the height it took; draw 0 measures only
static float text_wrap(int fid, float x, float y, float w, rgba c, const char *s, int draw) {
    char line[512] = "";
    float h = 0;
    const char *p = s;
    while (*p) {
        const char *e = p;
        while (*e && *e != ' ') e++;
        char cand[512];
        snprintf(cand, sizeof cand, "%s%s%.*s", line, line[0] ? " " : "", (int)(e - p), p);
        if (line[0] && text_w(fid, cand) > w) {
            if (draw && y + h > 110 && y + h < g_vh - 90) text(fid, x, y + h, c, line);
            h += FONT_PX[fid] * 1.35f;
            snprintf(line, sizeof line, "%.*s", (int)(e - p), p);
        } else snprintf(line, sizeof line, "%s", cand);
        p = *e ? e + 1 : e;
    }
    if (line[0]) { if (draw && y + h > 110 && y + h < g_vh - 90) text(fid, x, y + h, c, line); h += FONT_PX[fid] * 1.35f; }
    return h;
}
// Draws What's new: its headings and points, scrolled.
void draw_news(void) {
    news_load();
    draw_background();
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 10, 12, 18, 160);
    SDL_RenderFillRect(g_r, &all);
    draw_topbar();
    float y = 140 - g_news_top * 40.f, x = 120, w = g_vw - 240;
    const char *p = g_news;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char l[1024];
        snprintf(l, sizeof l, "%.*s", (int)(n < sizeof l - 1 ? n : sizeof l - 1), p);
        if (!strncmp(l, "## ", 3)) { y += 14; if (y > 110 && y < g_vh - 90) text(F_TITLE, x, y, C_ACCENT, l + 3); y += 56; }
        else if (!strncmp(l, "- ", 2)) { if (y > 110 && y < g_vh - 90) circle(x + 8, y + 15, 4, C_TEXT); y += text_wrap(F_BODY, x + 26, y, w - 26, C_TEXT, l + 2, 1) + 4; }
        else if (l[0]) y += text_wrap(F_BODY, x, y, w, C_DIM, l, 1) + 4;
        else y += 10;
        p = e ? e + 1 : p + n;
    }
    float hx = g_vw - 60;
    hint_left(&hx, "back", "Back", C_TEXT);
}
static shot_t *g_shots; int g_nshots, g_al_i, g_al_full;
static SDL_Texture *g_al_big; static int g_al_big_for = -1;
static SDL_Mutex *g_al_mu; static SDL_Thread *g_al_th; static int g_al_gen;
static void album_dir(char *out, size_t n) { nx_dir(1, "album", out, n); }
static int by_mtime(const void *a, const void *b) { long long x = ((const shot_t *)a)->mtime, y = ((const shot_t *)b)->mtime; return x < y ? 1 : x > y ? -1 : 0; }
// The Album's thread: makes the screenshots' small pictures, one at a time.
static int album_worker(void *arg) {
    (void)arg;
    for (;;) {
        SDL_LockMutex(g_al_mu);
        int gen = g_al_gen, k = -1;
        for (int i = 0; i < g_nshots; i++) if (g_shots[i].state == 0) { k = i; g_shots[i].state = 1; break; }
        char path[1200] = "";
        if (k >= 0) snprintf(path, sizeof path, "%s", g_shots[k].path);
        SDL_UnlockMutex(g_al_mu);
        if (k < 0) { SDL_Delay(50); continue; }
        int w, h, n;
        uint8_t *px = stbi_load(path, &w, &h, &n, 4), *small = NULL;
        int tw = 320, th = px ? (int)((long)320 * h / w) : 0;
        if (px) { small = img_down(px, w, h, tw, th); stbi_image_free(px); }
        SDL_LockMutex(g_al_mu);
        if (gen == g_al_gen && k < g_nshots) { g_shots[k].px = small, g_shots[k].w = tw, g_shots[k].h = th, g_shots[k].state = small ? 2 : 3; small = NULL; }
        SDL_UnlockMutex(g_al_mu);
        free(small);
    }
    return 0;
}
// Finds the screenshots (newest first) and starts making their small pictures.
void album_scan(void) {
    if (!g_al_mu) g_al_mu = SDL_CreateMutex();
    SDL_LockMutex(g_al_mu);
    for (int i = 0; i < g_nshots; i++) { free(g_shots[i].px); if (g_shots[i].t) SDL_DestroyTexture(g_shots[i].t); }
    free(g_shots), g_shots = NULL, g_nshots = 0, g_al_gen++;
    char d[1100];
    album_dir(d, sizeof d);
    DIR *dd = opendir(d);
    struct dirent *ge;
    int cap = 0;
    while (dd && (ge = readdir(dd))) {
        if (ge->d_name[0] == '.') continue;
        char gd[1300];
        snprintf(gd, sizeof gd, "%s/%s", d, ge->d_name);
        DIR *g = opendir(gd);
        struct dirent *fe;
        while (g && (fe = readdir(g))) {
            size_t l = strlen(fe->d_name);
            if (l < 5 || strcasecmp(fe->d_name + l - 4, ".png")) continue;
            if (g_nshots == cap) g_shots = realloc(g_shots, (size_t)(cap = cap ? 2 * cap : 32) * sizeof *g_shots);
            shot_t *s = &g_shots[g_nshots++];
            memset(s, 0, sizeof *s);
            snprintf(s->path, sizeof s->path, "%s/%s", gd, fe->d_name);
            snprintf(s->game, sizeof s->game, "%s", ge->d_name);
            struct stat st;
            if (!stat(s->path, &st)) s->mtime = (long long)st.st_mtime;
        }
        if (g) closedir(g);
    }
    if (dd) closedir(dd);
    if (g_nshots) qsort(g_shots, (size_t)g_nshots, sizeof *g_shots, by_mtime);
    SDL_UnlockMutex(g_al_mu);
    if (!g_al_th) g_al_th = SDL_CreateThread(album_worker, "album", NULL);
    if (g_al_i >= g_nshots) g_al_i = g_nshots ? g_nshots - 1 : 0;
    if (g_al_big) SDL_DestroyTexture(g_al_big), g_al_big = NULL, g_al_big_for = -1;
}
void avatar_choose_done(const char *path) { avatar_set(g_profs.cur, path); }
void album_delete(void) {
    if (g_al_i < 0 || g_al_i >= g_nshots) return;
    if (remove(g_shots[g_al_i].path)) { toast("The screenshot could not be deleted"); return; }
    album_scan();
    if (!g_nshots) g_al_full = 0;
}
// Draws the Album: the screenshots in a grid, or the one chosen large.
void draw_album(void) {
    draw_background();
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 10, 12, 18, 170);
    SDL_RenderFillRect(g_r, &all);
    draw_topbar();
    SDL_LockMutex(g_al_mu);
    for (int i = 0; i < g_nshots; i++) if (g_shots[i].state == 2 && !g_shots[i].t) { g_shots[i].t = tex_from_rgba(g_shots[i].px, g_shots[i].w, g_shots[i].h, 1); free(g_shots[i].px); g_shots[i].px = NULL; }
    SDL_UnlockMutex(g_al_mu);
    if (!g_nshots) {
        icon(K_ALBUM, 140, 300, 34, (rgba){70, 150, 245, 255});
        text(F_TITLE, 200, 262, C_TEXT, "No screenshots yet");
        text(F_BODY, 200, 318, C_DIM, "Press F12 in a game to take one: it shows here");
    } else if (g_al_full) {
        if (g_al_big_for != g_al_i) {   // the picture itself, large
            if (g_al_big) SDL_DestroyTexture(g_al_big), g_al_big = NULL;
            int w, h, n;
            uint8_t *px = stbi_load(g_shots[g_al_i].path, &w, &h, &n, 4);
            if (px) g_al_big = tex_from_rgba(px, w, h, 1), stbi_image_free(px);
            g_al_big_for = g_al_i;
        }
        if (g_al_big) {
            float tw, th;
            SDL_GetTextureSize(g_al_big, &tw, &th);
            float s = fminf((g_vw - 160) / tw, (g_vh - 220) / th);
            SDL_FRect d = {(g_vw - tw * s) / 2, 120, tw * s, th * s};
            SDL_RenderTexture(g_r, g_al_big, NULL, &d);
        }
    } else {
        const float tw = 260, th = 146, gap = 22, x0 = (g_vw - 4 * tw - 3 * gap) / 2;
        int top = g_al_i / 4 > 1 ? g_al_i / 4 - 1 : 0;
        for (int i = top * 4; i < g_nshots && i < top * 4 + 12; i++) {
            float x = x0 + (i % 4) * (tw + gap), y = 140 + (i / 4 - top) * (th + gap);
            if (i == g_al_i) rrect(x - 5, y - 5, tw + 10, th + 10, 10, C_ACCENT);
            if (g_shots[i].t) draw_cover(g_shots[i].t, x, y, tw, th, 255);
            else rrect(x, y, tw, th, 6, (rgba){255, 255, 255, 30});
        }
    }
    if (g_nshots) {
        char info[400], when[64];
        time_t t = (time_t)g_shots[g_al_i].mtime;
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(when, sizeof when, "%d %b %Y %H:%M", &tm);
        snprintf(info, sizeof info, "%s  \xc2\xb7  %s  \xc2\xb7  %d of %d", g_shots[g_al_i].game, when, g_al_i + 1, g_nshots);
        text(F_SMALL, 84, g_vh - 66, C_DIM, info);
    }
    float hx = g_vw - 60;
    hint_left(&hx, "back", "Back", C_TEXT);
    if (g_nshots) {
        hint_left(&hx, "ok", g_al_full ? "Close" : "View", C_TEXT);
        hint_left(&hx, "fav", "Open folder", C_TEXT);
        hint_left(&hx, "prof", "Delete", C_TEXT);
    }
}
// Handles a button in the Album (open one large, delete it, open its folder).
void album_act(int a) {
    if (a == A_BACK) { if (g_al_full) g_al_full = 0, sound(SND_BACK); else screen_open(SCR_DASH); return; }
    if (!g_nshots) { if (a == A_UP) { g_focus_top = TOP_ALBUM; moved(1); } return; }
    if (a == A_OK) { g_al_full = !g_al_full; sound(g_al_full ? SND_OPEN : SND_CLOSE); }
    else if (a == A_FAV) {   // its folder, in the file manager
        char d[1300];
        snprintf(d, sizeof d, "%s", g_shots[g_al_i].path);
        *strrchr(d, '/') = 0;
        open_folder(d);
        sound(SND_CONFIRM);
    }
    else if (a == A_PROF) { g_confirm_kind = 2; popup_open(POP_CONFIRM); }
    else if (a == A_LEFT) { moved(g_al_i > 0); if (g_al_i > 0) g_al_i--; }
    else if (a == A_RIGHT) { moved(g_al_i < g_nshots - 1); if (g_al_i < g_nshots - 1) g_al_i++; }
    else if (a == A_UP && !g_al_full) { if (g_al_i >= 4) g_al_i -= 4, moved(1); else { g_focus_top = TOP_ALBUM; moved(1); } }
    else if (a == A_DOWN && !g_al_full) { moved(g_al_i + 4 < g_nshots); if (g_al_i + 4 < g_nshots) g_al_i += 4; }
}
