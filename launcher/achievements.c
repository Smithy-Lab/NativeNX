// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, achievements: checked against the saves, the pop-ups, and a game's achievements page (nativenx.h lists the parts).
#include "nativenx.h"
static achg_t *g_achg; static int g_nachg, g_achg_cap;
static achq_t g_achq[24]; static int g_nachq;
static uint64_t g_achq_t0, g_ach_next;
static SDL_Texture *g_achq_tex;
int g_ach_seq, g_still;   // g_still: a picture of the window (--screenshot): a pop-up drawn where it rests
uint64_t ach_pid(const game_t *g) {   // the program ID; a game without one: from its folder's name
    if (g->program_id) return g->program_id;
    const char *b = strrchr(g->dir, '/');
    return 0xFF00000000000000ull | ach_crc32(b ? b + 1 : g->dir);
}
static void game_base(const game_t *g, char *out, size_t n) {   // the game's folder name, as file names use it
    snprintf(out, n, "%s", strrchr(g->dir, '/') ? strrchr(g->dir, '/') + 1 : g->dir);
    for (char *c = out; *c; c++) if (*c == '/' || *c == ' ') *c = '_';
}
static int count_shots(const game_t *g) {   // its screenshots in the album
    char base[256], sub[300], d[1400];
    game_base(g, base, sizeof base);
    snprintf(sub, sizeof sub, "album/%s", base);
    nx_dir(1, sub, d, sizeof d);
    DIR *dd = opendir(d);
    struct dirent *e;
    int n = 0;
    while (dd && (e = readdir(dd))) { size_t l = strlen(e->d_name); n += l > 4 && !strcasecmp(e->d_name + l - 4, ".png"); }
    if (dd) closedir(dd);
    return n;
}
// A game's achievements for the current profile (its list and saves), loaded the first time.
achg_t *ach_game(int gi) {
    if (gi < 0 || gi >= g_ngames || !g_profs.n) return NULL;
    int prof = g_profs.p[g_profs.cur].id;
    for (int i = 0; i < g_nachg; i++) if (g_achg[i].prof == prof && !strcmp(g_achg[i].dir, g_games[gi].dir)) return &g_achg[i];
    if (g_nachg == g_achg_cap) g_achg = realloc(g_achg, (size_t)(g_achg_cap = g_achg_cap ? 2 * g_achg_cap : 16) * sizeof *g_achg);
    achg_t *a = &g_achg[g_nachg++];
    memset(a, 0, sizeof *a);
    snprintf(a->dir, sizeof a->dir, "%s", g_games[gi].dir);
    a->prof = prof;
    ach_load(g_games[gi].program_id, g_data, &a->set);
    a->st = calloc((size_t)(a->set.n ? a->set.n : 1), sizeof *a->st);
    for (int i = 0; i < a->set.n; i++) a->st[i].state = -2;
    return a;
}
static int game_of(const achg_t *a) { for (int i = 0; i < g_ngames; i++) if (!strcmp(g_games[i].dir, a->dir)) return i; return -1; }
// What a game's conditions can read: its time played (this session too), its saves, the profile's other unlocks.
void ach_ctx_for(int gi, achg_t *a, ach_ctx_t *c) {
    play_t *p = plays_get(&g_plays, &g_games[gi], 0);
    double secs = p ? (double)p->secs : 0;
    if (g_child && g_play_gi == gi) secs += play_ms() / 1000.0;   // this session so far
    c->saves = &a->sv, c->play_hours = secs / 3600, c->screenshots = count_shots(&g_games[gi]);
    c->launched = p && p->last > 0, c->favourite = is_fav(gi);
}
static rgba tier_color(int points) {   // bronze, silver, gold, platinum
    return points >= 90 ? (rgba){170, 215, 255, 255} : points >= 50 ? (rgba){255, 200, 60, 255} : points >= 20 ? (rgba){200, 205, 215, 255} : (rgba){215, 140, 80, 255};
}
// the pop-up's picture, drawn here on the CPU (it goes to the game's window too): 880 x 184, straight alpha
static void px_over(uint8_t *p, rgba c, float cov) {
    float a = c.a / 255.f * cov;
    if (a <= 0.002f) return;
    float da = p[3] / 255.f, oa = a + da * (1 - a);
    uint8_t cc[3] = {c.r, c.g, c.b};
    for (int k = 0; k < 3; k++) p[k] = (uint8_t)((cc[k] * a + p[k] * da * (1 - a)) / oa + 0.5f);
    p[3] = (uint8_t)(oa * 255 + 0.5f);
}
// Draws a shape into a pixel buffer (for the achievement pop-up shown in the game's window).
static void img_shape(uint8_t *px, int W, int H, int k, float x, float y, float w, float h, float rad, rgba c) {
    float unit = k == K_RRECT ? 1 : h / 2, wu = w / unit, hu = h / unit, r = rad / unit;
    for (int yy = (int)fmaxf(0, floorf(y)); yy < (int)fminf(H, ceilf(y + h)); yy++)
        for (int xx = (int)fmaxf(0, floorf(x)); xx < (int)fminf(W, ceilf(x + w)); xx++) {
            float ux = (xx + 0.5f - x) / unit - wu / 2, uy = (yy + 0.5f - y) / unit - hu / 2;
            float d = shape_d(k, ux, uy, wu, hu, r) * unit;
            px_over(px + 4 * ((size_t)yy * W + xx), c, fminf(1, fmaxf(0, 0.5f - d)));
        }
}
// Draws text into a pixel buffer; its width (draw 0: measures only).
static float img_text(uint8_t *px, int W, int H, int bold, float size, float x, float base, rgba c, const char *s, int draw) {
    const stbtt_fontinfo *fi = bold && g_ttf_bold ? &g_fi_bold : &g_fi;
    if (!g_ttf) return 0;
    float sc = stbtt_ScaleForPixelHeight(fi, size), pen = x;
    uint32_t prev = 0;
    while (*s) {
        uint32_t cp = utf8_next(&s);
        if (prev) pen += sc * stbtt_GetCodepointKernAdvance(fi, (int)prev, (int)cp);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(fi, (int)cp, &adv, &lsb);
        if (draw) {
            int x0, y0, x1, y1;
            float sx = pen - floorf(pen);
            stbtt_GetCodepointBitmapBoxSubpixel(fi, (int)cp, sc, sc, sx, 0, &x0, &y0, &x1, &y1);
            int gw = x1 - x0, gh = y1 - y0;
            if (gw > 0 && gh > 0) {
                uint8_t *g = calloc((size_t)gw * gh, 1);
                stbtt_MakeCodepointBitmapSubpixel(fi, g, gw, gh, gw, sc, sc, sx, 0, (int)cp);
                for (int j = 0; j < gh; j++)
                    for (int i = 0; i < gw; i++) {
                        int X = (int)floorf(pen) + x0 + i, Y = (int)base + y0 + j;
                        if (X >= 0 && Y >= 0 && X < W && Y < H && g[j * gw + i]) px_over(px + 4 * ((size_t)Y * W + X), c, g[j * gw + i] / 255.f);
                    }
                free(g);
            }
        }
        pen += sc * adv;
        prev = cp;
    }
    return pen - x;
}
// The achievement pop-up as a picture (its trophy, name and points), for the game's window.
static uint8_t *ach_image(const achg_t *a, int di, int *W_, int *H_) {
    const int W = 880, H = 184;
    uint8_t *px = calloc((size_t)W * H, 4);
    const ach_def_t *d = &a->set.a[di];
    img_shape(px, W, H, K_RRECT, 0, 0, W, H, 34, (rgba){78, 84, 100, 245});
    img_shape(px, W, H, K_RRECT, 3, 3, W - 6, H - 6, 31, (rgba){24, 27, 36, 245});
    // the game's picture, a rounded square at the left
    int gi = game_of(a);
    int iw = 0, ih = 0, n;
    uint8_t *ic = gi >= 0 && g_games[gi].icon[0] ? stbi_load(g_games[gi].icon, &iw, &ih, &n, 4) : NULL;
    const float bx = 24, by = 24, bs = 136;
    if (ic) {
        for (int y = 0; y < (int)bs; y++)
            for (int x = 0; x < (int)bs; x++) {   // the middle square of the picture, sampled at the pixel's centre
                int m = iw < ih ? iw : ih, sx = (iw - m) / 2 + (int)((x + 0.5f) * m / bs), sy = (ih - m) / 2 + (int)((y + 0.5f) * m / bs);
                const uint8_t *q = ic + 4 * ((size_t)sy * iw + sx);
                float dd = shape_d(K_RRECT, x + 0.5f - bs / 2, y + 0.5f - bs / 2, bs, bs, 20);
                px_over(px + 4 * ((size_t)(by + y) * W + (size_t)(bx + x)), (rgba){q[0], q[1], q[2], q[3]}, fminf(1, fmaxf(0, 0.5f - dd)));
            }
        stbi_image_free(ic);
    } else {
        rgba tc = gi >= 0 ? title_color(g_games[gi].title) : C_ACCENT;
        img_shape(px, W, H, K_RRECT, bx, by, bs, bs, 20, tc);
        img_shape(px, W, H, K_TROPHY, bx + 34, by + 34, 68, 68, 0, (rgba){255, 255, 255, 230});
    }
    rgba tc = tier_color(d->points);
    img_text(px, W, H, 0, 32, 190, 70, (rgba){185, 192, 208, 255}, "You have earned an achievement.", 1);
    img_shape(px, W, H, K_TROPHY, 190, 94, 50, 50, 0, tc);
    char pts[24];
    snprintf(pts, sizeof pts, "%d", d->points);
    float pw = img_text(px, W, H, 1, 36, 0, 0, tc, pts, 0);
    img_text(px, W, H, 1, 36, W - 32 - pw, 134, tc, pts, 1);
    char nm[120];
    snprintf(nm, sizeof nm, "%s", d->name);
    float maxw = W - 32 - pw - 24 - 254;
    while (strlen(nm) > 4 && img_text(px, W, H, 1, 40, 0, 0, C_TEXT, nm, 0) > maxw) { size_t l = strlen(nm); nm[l - 4] = 0; strcat(nm, "..."); }
    img_text(px, W, H, 1, 40, 254, 134, (rgba){255, 255, 255, 255}, nm, 1);
    *W_ = W, *H_ = H;
    return px;
}
// a new one: the chime, and the pop-up in the game's window (while it runs) or here
static void ach_notify(int ai, int di) {
    sound(SND_ACH);
    achg_t *a = &g_achg[ai];
    int gi = game_of(a);
    if (g_ach_fd >= 0 && g_child && gi == g_play_gi) {
        int w, h;
        uint8_t *px = ach_image(a, di, &w, &h);
        char path[1100];
        snprintf(path, sizeof path, "%s/nxlinux-ach-%d-%d.rgba", nx_runtime_dir(), (int)getpid(), g_ach_seq++);
        FILE *f = fopen(path, "wb");
        if (f) {   // NXLIMG01, width, height (32-bit little-endian), then RGBA rows, straight alpha
            uint32_t wh[2] = {(uint32_t)w, (uint32_t)h};
            fwrite("NXLIMG01", 1, 8, f), fwrite(wh, 4, 2, f), fwrite(px, 4, (size_t)w * h, f);
            fclose(f);
            char line[1200];
            int l = snprintf(line, sizeof line, "%s\n", path);
            if (write(g_ach_fd, line, (size_t)l) != l) LOG("the game's window did not take the pop-up");
        }
        free(px);
        return;
    }
    if (g_nachq < (int)(sizeof g_achq / sizeof *g_achq)) g_achq[g_nachq++] = (achq_t){ai, di};
}
// Checks a game's achievements: the new ones unlocked, saved, and shown (a chime and a pop-up); how many.
int ach_check(int gi, int notify) {
    achg_t *a = ach_game(gi);
    if (!a || !a->set.n) return 0;
    char save[1100];
    profile_save_dir(&g_profs, g_profs.cur, &g_games[gi], save, sizeof save);
    if (ach_saves_read(&a->set, save, &a->sv)) LOG("achievements: %s: %d save slot%s read", g_games[gi].title, a->sv.n, a->sv.n == 1 ? "" : "s");
    ach_ctx_t c;
    ach_ctx_for(gi, a, &c);
    ach_eval(&a->set, &c, a->st);
    uint64_t pid = ach_pid(&g_games[gi]);
    int n = 0;
    for (int i = 0; i < a->set.n; i++) {
        if (a->st[i].state != 1 || ach_unlocked(&g_ach, pid, a->set.a[i].id)) continue;
        ach_unlock(&g_ach, pid, a->set.a[i].id, a->set.a[i].points, (long long)time(NULL));
        LOG("achievement unlocked: %s: %s (%d points)", g_games[gi].title, a->set.a[i].name, a->set.a[i].points);
        if (notify) ach_notify((int)(a - g_achg), i);
        n++;
    }
    if (n) ach_store_save(g_profs.p[g_profs.cur].id, &g_ach);
    return n;
}
void ach_tick(void) {   // while a game runs: its saves and time looked at every 3 s
    if (!g_child || g_suspended || g_play_gi < 0 || SDL_GetTicks() < g_ach_next) return;
    g_ach_next = SDL_GetTicks() + 3000;
    ach_check(g_play_gi, 1);
}
// the pop-ups shown here, one after another: in from the right, 4.5 s, out
void draw_ach_popups(void) {
    if (!g_nachq) return;
    uint64_t now = SDL_GetTicks();
    if (!g_achq_tex) {
        int w, h;
        uint8_t *px = ach_image(&g_achg[g_achq[0].ai], g_achq[0].di, &w, &h);
        g_achq_tex = tex_from_rgba(px, w, h, 1);
        free(px);
        g_achq_t0 = g_still ? now - 1000 : now;
    }
    float t = (now - g_achq_t0) / 1000.f, in = fminf(1, t / 0.3f), out = t > 4.8f ? fminf(1, (t - 4.8f) / 0.3f) : 0;
    float e = 1 - (1 - in) * (1 - in) * (1 - in), slide = (1 - e) + out * out;
    const float w = 440, h = 92;
    SDL_FRect dst = {g_vw - w - 24 + slide * (w + 40), 24, w, h};
    if (g_achq_tex) SDL_RenderTexture(g_r, g_achq_tex, NULL, &dst);
    if (t > 5.1f) {
        if (g_achq_tex) SDL_DestroyTexture(g_achq_tex), g_achq_tex = NULL;
        memmove(g_achq, g_achq + 1, (size_t)(--g_nachq) * sizeof *g_achq);
    }
}

// ---------- a game's achievements ----------
// The game's picture and title, how many are unlocked and their points, then the list: the unlocked ones first (the
// newest first), then the rest in the list's order, each with its trophy (bronze, silver, gold, platinum by points),
// its points, and when it was unlocked or how far along it is. Hidden ones show their name once unlocked
static int g_av_gi = -1, g_av_row, g_av_top;   // the game shown, the row selected, the first row shown
static int g_av_order[256], g_av_n;
// The order on a game's achievements page: unlocked first (newest first), then the others.
void ach_page_order(achg_t *a, uint64_t pid) {
    g_av_n = 0;
    int un[256], nu = 0;
    for (int i = 0; i < a->set.n && i < 256; i++) if (ach_unlocked(&g_ach, pid, a->set.a[i].id)) un[nu++] = i;
    for (int i = 0; i < nu; i++)   // newest first
        for (int j = i + 1; j < nu; j++) if (ach_unlocked(&g_ach, pid, a->set.a[un[j]].id) > ach_unlocked(&g_ach, pid, a->set.a[un[i]].id)) { int t = un[i]; un[i] = un[j]; un[j] = t; }
    for (int i = 0; i < nu; i++) g_av_order[g_av_n++] = un[i];
    for (int i = 0; i < a->set.n && i < 256; i++) if (!ach_unlocked(&g_ach, pid, a->set.a[i].id)) g_av_order[g_av_n++] = i;
}
void ach_page_open(int gi) {
    if (gi < 0 || gi >= g_ngames) return;
    ach_check(gi, 1);
    screen_open(SCR_ACH);
    g_av_gi = gi, g_av_row = 0, g_av_top = 0;
}
static int av_rows_shown(void) { int n = (int)((g_vh - 100 - AV_Y0) / AV_ROW); return n < 1 ? 1 : n; }
// the rows of a game's achievements (ach_page_order first): from y0, nshow rows of AV_ROW, sel highlighted (-1 none),
// scrolled so it shows (*top: the first row shown)
void draw_ach_rows(achg_t *a, uint64_t pid, float x0, float y0, float w0, int nshow, int sel_row, int *top) {
    if (sel_row >= 0 && sel_row < *top) *top = sel_row;
    if (sel_row >= nshow + *top) *top = sel_row - nshow + 1;
    if (*top > g_av_n - nshow) *top = g_av_n - nshow > 0 ? g_av_n - nshow : 0;
    for (int r = 0; r < nshow && *top + r < g_av_n; r++) {
        int k = *top + r, i = g_av_order[k];
        const ach_def_t *d = &a->set.a[i];
        const ach_state_t *s = &a->st[i];
        float y = y0 + r * AV_ROW, x = x0, w = w0;
        long long when = ach_unlocked(&g_ach, pid, d->id);
        int sel = k == sel_row, hid = d->hidden && !when;
        rrect(x, y, w, AV_ROW - 8, 14, sel ? (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 70} : (rgba){255, 255, 255, (uint8_t)(when ? 26 : 14)});
        rgba tc = tier_color(d->points);
        if (!when) tc = (rgba){120, 126, 140, 200};
        circle(x + 38, y + 34, 24, (rgba){0, 0, 0, 90});
        icon(K_TROPHY, x + 38, y + 34, 15, tc);
        float tx = x + 80;
        SDL_Rect cl = {(int)tx, 0, (int)(w - 360), (int)g_vh};
        SDL_SetRenderClipRect(g_r, &cl);
        float nw = text(F_BODYB, tx, y + 8, when ? C_TEXT : (rgba){C_TEXT.r, C_TEXT.g, C_TEXT.b, 200}, hid ? "Hidden achievement" : d->name);
        if (d->dlc) { float cx = tx + nw + 12; rrect(cx, y + 11, 50, 24, 12, (rgba){255, 255, 255, 40}); text(F_SMALL, cx + 9, y + 12, C_DIM, "DLC"); }
        text(F_SMALL, tx, y + 40, C_DIM, hid ? "Keep playing to find out" : d->desc);
        SDL_SetRenderClipRect(g_r, NULL);
        // right: the points, then when, or how far
        char p[24];
        snprintf(p, sizeof p, "%d", d->points);
        float pw = text_w(F_BODYB, p);
        text(F_BODYB, x + w - 24 - pw, y + 8, when ? tier_color(d->points) : C_DIM, p);
        char r2[64] = "";
        if (when) { time_t t = (time_t)when; struct tm tm; localtime_r(&t, &tm); strftime(r2, sizeof r2, "%d %b %Y", &tm); }
        else if (s->state == -1) snprintf(r2, sizeof r2, "Not tracked in this version");
        else if (s->state == -2 && !d->general) snprintf(r2, sizeof r2, "Waiting for a save");
        else if (s->target > 1 && !hid) {   // a counter: a bar
            float bx = x + w - 24 - 200, by = y + 48, f = fminf(1, (float)(s->value / s->target));
            rrect(bx, by, 200, 8, 4, (rgba){255, 255, 255, 40});
            if (f > 0) rrect(bx, by, fmaxf(8, 200 * f), 8, 4, C_ACCENT);
            char v[48];
            if (s->target < 10 && s->target != (int)s->target) snprintf(v, sizeof v, "%.1f / %g", s->value, s->target);
            else snprintf(v, sizeof v, "%.0f / %.0f", fmin(s->value, s->target), s->target);
            text(F_SMALL, bx - 14 - text_w(F_SMALL, v), y + 38, C_DIM, v);
        }
        if (r2[0]) text(F_SMALL, x + w - 24 - text_w(F_SMALL, r2), y + 40, C_DIM, r2);
    }
}
// Draws a game's achievements page: progress, each achievement with its state and points.
void draw_ach_page(void) {
    draw_background();
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 10, 12, 18, 175);
    SDL_RenderFillRect(g_r, &all);
    draw_topbar();
    achg_t *a = ach_game(g_av_gi);
    if (!a) return;
    const game_t *g = &g_games[g_av_gi];
    uint64_t pid = ach_pid(g);
    ach_page_order(a, pid);
    draw_tile(g_av_gi, 84, 120, 96, 0, 0);
    SDL_Rect clip = {200, 0, (int)(g_vw - 260), (int)g_vh};
    SDL_SetRenderClipRect(g_r, &clip);
    text(F_TITLE, 204, 116, C_TEXT, g->title);
    SDL_SetRenderClipRect(g_r, NULL);
    int got = 0, pts = 0;
    for (int i = 0; i < a->set.n; i++) if (ach_unlocked(&g_ach, pid, a->set.a[i].id)) got++, pts += a->set.a[i].points;
    char st[200];
    snprintf(st, sizeof st, "%d of %d unlocked  \xc2\xb7  %d of %d points", got, a->set.n, pts, a->set.total_points);
    icon(K_TROPHY, 216, 182, 12, (rgba){255, 200, 60, 255});
    text(F_BODY, 236, 168, C_DIM, st);
    float bw = fminf(520, g_vw - 300);
    rrect(204, 204, bw, 8, 4, (rgba){255, 255, 255, 40});
    if (a->set.total_points) rrect(204, 204, fmaxf(8, bw * pts / a->set.total_points), 8, 4, C_ACCENT);
    if (!a->set.n) { text(F_BODY, 84, AV_Y0 + 20, C_DIM, "No achievements for this game"); return; }
    int nshow = av_rows_shown();
    draw_ach_rows(a, pid, 84, AV_Y0, g_vw - 168, nshow, g_focus_top < 0 ? g_av_row : -1, &g_av_top);
    if (g_av_top + nshow < g_av_n) text(F_SMALL, g_vw / 2 - 40, g_vh - 96, C_DIM, "\xe2\x86\x93 more");
    float hx = g_vw - 60;
    hint_left(&hx, "back", "Back", C_TEXT);
}
void ach_page_act(int a) {
    if (a == A_BACK) { screen_open(SCR_DASH); return; }
    if (a == A_UP) { if (g_av_row > 0) g_av_row--, moved(1); else { g_focus_top = TOP_PROFILE; moved(1); } }
    else if (a == A_DOWN) { moved(g_av_row < g_av_n - 1); if (g_av_row < g_av_n - 1) g_av_row++; }
    else if (a == A_LEFT || a == A_RIGHT || a == A_OK) moved(0);
}
