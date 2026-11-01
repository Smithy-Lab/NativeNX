// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, profiles: the row's order, avatars, the profile page, the profiles window, name entry (nativenx.h lists the parts).
#include "nativenx.h"
// ---------- profiles: the row's order, avatars, the profile page, the profiles window, names ----------
// The row shows the games last played first (the current profile's), then the ones never played by title; with the
// favourites filter, the favourites only. keep: the game (an index into g_games before) that stays selected, -1 the
// one selected now
void order_games(int keep) {
    if (!g_ngames) { g_nshow = 0; return; }
    const char *keep_dir = keep >= 0 && keep < g_ngames ? g_games[keep].dir : keep == -1 && g_sel < g_ngames ? g_games[g_sel].dir : NULL;   // -2: the first
    char kd[1024] = "";
    if (keep_dir) snprintf(kd, sizeof kd, "%s", keep_dir);
    char pd[1024], ld[1024];   // the game running (or waiting at Home) and the one loading: they follow their folders
    play_dirs_keep(pd, ld);
    int *perm = malloc((size_t)g_ngames * sizeof *perm);
    plays_order(&g_plays, g_games, g_ngames, perm);
    int nfav = 0;
    if (g_fav_only) {   // the favourites first (in the same order), the rest after them
        int *p2 = malloc((size_t)g_ngames * sizeof *p2), k = 0;
        for (int i = 0; i < g_ngames; i++) if (is_fav(perm[i])) p2[k++] = perm[i];
        nfav = k;
        for (int i = 0; i < g_ngames; i++) if (!is_fav(perm[i])) p2[k++] = perm[i];
        free(perm), perm = p2;
    }
    game_t *ng = malloc((size_t)g_ngames * sizeof *ng);
    pic_t *np = calloc((size_t)g_ngames, sizeof *np);
    for (int i = 0; i < g_ngames; i++) ng[i] = g_games[perm[i]], np[i] = g_pics[perm[i]];
    free(g_games), free(g_pics), free(perm);
    g_games = ng, g_pics = np;
    SDL_LockMutex(g_bg_mu);
    g_bg_gen++;   // backgrounds asked for under the old order are dropped, and asked for again
    SDL_UnlockMutex(g_bg_mu);
    for (int i = 0; i < g_ngames; i++) if (!g_pics[i].bg) g_pics[i].asked = 0;
    g_sel = 0;
    for (int i = 0; i < g_ngames; i++) if (kd[0] && !strcmp(g_games[i].dir, kd)) g_sel = i;
    g_nshow = g_fav_only ? nfav : g_ngames;
    if (g_sel >= g_nshow) g_sel = 0;
    g_scroll = (float)g_sel, g_prev_bg = -1;
    play_dirs_follow(pd, ld);
}
// Makes the selected game a favourite, or not.
void fav_toggle(void) {
    if (!g_nshow || g_sel >= g_ngames) return;
    play_t *p = plays_get(&g_plays, &g_games[g_sel], 1);
    p->fav = !p->fav;
    plays_save(g_profs.p[g_profs.cur].id, &g_plays);
    sound(SND_TOGGLE);
    ach_check(g_sel, 1);   // a favourite: one of the general achievements
    if (g_fav_only && !p->fav) order_games(-1);   // no longer one: out of the favourites row
}
void fav_filter(void) {
    g_fav_only = !g_fav_only;
    order_games(-1);
    sound(SND_TOGGLE);
}
static void profile_use(int i) {   // the profile in use from now: its time and favourites, its saves
    if (i < 0 || i >= g_profs.n) return;
    g_profs.cur = i;
    profiles_save(&g_profs);
    plays_load(g_profs.p[i].id, &g_plays);
    ach_store_load(g_profs.p[i].id, &g_ach);
    order_games(-2);
    LOG("profile: %s", g_profs.p[i].name);
}

// avatars: the chosen picture, square and round; without one, the name's first letter on a colour from the name
static SDL_Texture *g_av_tex[PROFILE_MAX]; static int g_av_for[PROFILE_MAX];   // the texture and the profile id it shows
// A profile's avatar picture as a texture (made again when the profile changed).
static SDL_Texture *avatar_tex(int i) {
    if (i < 0 || i >= g_profs.n || !g_profs.p[i].has_avatar) return NULL;
    if (g_av_tex[i] && g_av_for[i] == g_profs.p[i].id) return g_av_tex[i];
    if (g_av_tex[i]) SDL_DestroyTexture(g_av_tex[i]), g_av_tex[i] = NULL;
    char path[1100];
    profile_avatar_path(g_profs.p[i].id, path, sizeof path);
    int w, h, n;
    uint8_t *px = stbi_load(path, &w, &h, &n, 4);
    if (!px) return NULL;
    if (w != h) {   // the middle square (avatar_set writes square ones)
        int s0 = w < h ? w : h;
        uint8_t *sq = malloc((size_t)s0 * s0 * 4);
        for (int y = 0; y < s0; y++) memcpy(sq + (size_t)y * s0 * 4, px + ((size_t)(y + (h - s0) / 2) * w + (w - s0) / 2) * 4, (size_t)s0 * 4);
        stbi_image_free(px);
        px = sq, w = h = s0;
    }
    for (int y = 0; y < h; y++)   // round: alpha outside the circle, a soft edge
        for (int x = 0; x < w; x++) {
            float d = hypotf(x + 0.5f - w / 2.f, y + 0.5f - h / 2.f) - w / 2.f + 1;
            float a = fminf(1, fmaxf(0, 0.5f - d));
            px[((size_t)y * w + x) * 4 + 3] = (uint8_t)(px[((size_t)y * w + x) * 4 + 3] * a);
        }
    g_av_tex[i] = tex_from_rgba(px, w, h, 1), g_av_for[i] = g_profs.p[i].id;
    stbi_image_free(px);
    return g_av_tex[i];
}
// Draws a profile's avatar in a circle, or its initial on a colour when it has no picture.
static void draw_avatar(int i, float cx, float cy, float r) {
    SDL_Texture *t = avatar_tex(i);
    if (t) { SDL_FRect d = {cx - r, cy - r, 2 * r, 2 * r}; SDL_SetTextureAlphaMod(t, 255); SDL_RenderTexture(g_r, t, NULL, &d); return; }
    const char *nm = i >= 0 && i < g_profs.n ? g_profs.p[i].name : "?";
    circle(cx, cy, r, i >= 0 && i < g_profs.n ? title_color(nm) : (rgba){200, 200, 200, 255});
    char ini[8] = {(char)toupper((unsigned char)nm[0]), 0};
    int fid = r > 60 ? F_BIG : r > 30 ? F_TITLE : F_BODY;
    text(fid, cx - text_w(fid, ini) / 2, cy - FONT_PX[fid] * 0.62f, (rgba){255, 255, 255, 245}, ini);
}
// a chosen picture becomes the profile's avatar: the middle square, 256 x 256, as PNG
void avatar_set(int i, const char *src) {
    if (i < 0 || i >= g_profs.n) return;
    int w, h, n;
    uint8_t *px = stbi_load(src, &w, &h, &n, 4);
    if (!px) { toast("That picture could not be read (PNG or JPEG)"); return; }
    int s0 = w < h ? w : h;
    uint8_t *sq = malloc((size_t)s0 * s0 * 4);
    for (int y = 0; y < s0; y++) memcpy(sq + (size_t)y * s0 * 4, px + ((size_t)(y + (h - s0) / 2) * w + (w - s0) / 2) * 4, (size_t)s0 * 4);
    stbi_image_free(px);
    int ds = s0 < 256 ? s0 : 256;
    uint8_t *out = s0 > ds ? img_down(sq, s0, s0, ds, ds) : sq;
    char path[1100];
    profile_avatar_path(g_profs.p[i].id, path, sizeof path);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    if (!stbi_write_png(path, ds, ds, 4, out, ds * 4)) toast("The avatar could not be saved");
    else g_profs.p[i].has_avatar = 1, g_av_for[i] = 0;
    if (out != sq) free(out);
    free(sq);
    sound(SND_CONFIRM);
}

// screens besides the dashboard and the settings, and the small windows over them
int g_pp_row, g_pp_btn, g_pp_game;   // the profile page: the row (0 its buttons, 1 recently played), the button, the game
char g_name_buf[24]; int g_name_mode, g_name_key, g_name_shift = 1;   // name entry: 0 a new profile, 1 rename
// g_confirm_kind: 1 delete the profile, 2 delete the screenshot, 3 close the running game, 4 an older version, 5 a DLC
// off (both: Cancel / Do not copy / Copy the save first), 6 restore a backup
void popup_open(int kind) {
    if (g_child && (kind == POP_PROFILES || (kind == POP_CONFIRM && g_confirm_kind == 1))) {   // the game plays as this profile (its saves)
        char m[400];
        snprintf(m, sizeof m, "%s is still running as %s: close it to change profiles", g_play_gi >= 0 ? g_games[g_play_gi].title : "A game", g_profs.p[g_profs.cur].name);
        toast(m);
        return;
    }
    g_popup = kind, g_pop_i = 0;
    if (kind == POP_PROFILES) g_pop_i = g_profs.cur;
    if (kind == POP_NAME) { g_name_key = 0, g_name_shift = g_name_buf[0] == 0; SDL_StartTextInput(g_win); }
    if (kind == POP_CONFIRM) g_confirm_yes = 0, g_confirm_n = 2;
    sound(SND_OPEN);
}
void popup_close(void) {
    if (g_popup == POP_NAME) SDL_StopTextInput(g_win);
    g_popup = POP_NONE;
    sound(SND_CLOSE);
}
void screen_open(int s) {
    g_screen = s; g_focus_top = -1; g_pp_row = 0, g_pp_btn = 0, g_pp_game = 0, g_news_top = 0;
    if (s == SCR_ALBUM) album_scan();
    sound(s ? SND_OPEN : SND_CLOSE);
}

// the recently played games of the current profile, newest first: indices into g_games
int recent_games(int *out, int max) {
    int *perm = malloc((size_t)(g_ngames ? g_ngames : 1) * sizeof *perm), n = 0;
    plays_order(&g_plays, g_games, g_ngames, perm);
    for (int i = 0; i < g_ngames && n < max; i++) { play_t *p = plays_get(&g_plays, &g_games[perm[i]], 0); if (p && p->last) out[n++] = perm[i]; }
    free(perm);
    return n;
}
void hours_text(long long secs, char *out, size_t n) {
    if (secs < 3600) snprintf(out, n, "%lld minute%s", secs / 60, secs / 60 == 1 ? "" : "s");
    else if (secs < 36000) snprintf(out, n, "%.1f hours", secs / 3600.0);
    else snprintf(out, n, "%lld hours", secs / 3600);
}

// the top bar: the avatar (the profiles), then What's new, Album, Controllers, Settings, Exit
void draw_topbar(void) {
    if (g_focus_top == TOP_PROFILE) circle(70, 62, 31, C_ACCENT);
    draw_avatar(g_profs.cur, 70, 62, 27);
    rrect(117, 40, 2, 44, 1, (rgba){255, 255, 255, 120});
    static const int ic[TOP_N] = {0, K_NEWS, K_ALBUM, K_JOY, K_GEAR, K_POWER};
    static const rgba col[TOP_N] = {{0}, {240, 92, 84, 255}, {70, 150, 245, 255}, {235, 240, 250, 255}, {255, 255, 255, 240}, {255, 255, 255, 240}};
    for (int i = 1; i < TOP_N; i++) {
        // each drawn to the same size by eye: the frames 27 x 22, the pad 28 x 18, the gear and the power sign 26 high
        // (their shapes fill the icon's square differently), and centred on the bar's line
        static const float sc[TOP_N] = {0, 1, 1, 1, 0.87f, 0.94f}, oy[TOP_N] = {0, 0.25f, 0.25f, -2, 0.25f, 2};
        if (g_focus_top == i) circle(top_x(i), 62, 26, (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 90});
        if (i == TOP_PADS) {   // the controllers: Zacksly's Pro Controller (launcher/data/prompts, CC BY 3.0), 30 wide
            SDL_Texture *t = prompt_tex("zacksly_pro_controller", 30);
            if (t) {
                float tw, th;
                SDL_GetTextureSize(t, &tw, &th);
                SDL_FRect d = {top_x(i) - tw / g_s / 2, 62 - th / g_s / 2, tw / g_s, th / g_s};
                SDL_SetTextureColorMod(t, col[i].r, col[i].g, col[i].b);
                SDL_SetTextureAlphaMod(t, col[i].a);
                SDL_RenderTexture(g_r, t, NULL, &d);
                continue;
            }
        }
        icon(ic[i], top_x(i), 62 + oy[i], 15 * sc[i], col[i]);
    }
    if (g_focus_top >= 0) {
        const char *l = g_focus_top == TOP_PROFILE && g_screen == SCR_PROFILE ? "Home" : TOP_LABEL[g_focus_top];
        text(F_SMALL, top_x(g_focus_top) - text_w(F_SMALL, l) / 2, 94, C_TEXT, l);
    }
    draw_status(g_vw - 60, 46);
}
// Does what a top bar button is for (Profile, What's new, Album, Controllers, Settings, Exit).
void top_do(int i) {
    switch (i) {
    case TOP_PROFILE: screen_open(g_screen == SCR_PROFILE ? SCR_DASH : SCR_PROFILE); break;   // the avatar: the profile page, and from it back (X / P is the quick switch)
    case TOP_NEWS: screen_open(SCR_NEWS); break;
    case TOP_ALBUM: screen_open(SCR_ALBUM); break;
    case TOP_PADS: settings_open(1); g_set_cat = 3; break;
    case TOP_SETTINGS: settings_open(1); break;
    default: sound(SND_CLOSE); SDL_Delay(250); g_quit = 1; break;
    }
}

// ---------- the profile page ----------
// The profile's picture large, its name, its play (time, games, since when), then its recently played games with their
// time. A later version adds an online status here (g_online_shown, off: there is no online service)
static const int g_online_shown = 0;
const char *const PP_BTN[4] = {"Change avatar", "Rename", "Switch profile", "Delete profile"};
// Draws the profile page: the avatar and name, time played, the recent games, the buttons.
void draw_profile_page(void) {
    draw_background();
    draw_topbar();
    const profile_t *p = &g_profs.p[g_profs.cur];
    circle(215, 265, 98, C_ACCENT);
    draw_avatar(g_profs.cur, 215, 265, 90);
    text(F_BIG, 345, 180, C_TEXT, p->name);
    float y = 262;
    if (g_online_shown) { circle(355, y + 14, 9, (rgba){80, 220, 100, 255}); text(F_BODY, 374, y, C_TEXT, "Online"); y += 40; }
    char st[200], hrs[48], since[48];
    hours_text(plays_total(&g_plays), hrs, sizeof hrs);
    time_t t = (time_t)p->since;
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(since, sizeof since, "%d %B %Y", &tm);
    snprintf(st, sizeof st, "%s played  \xc2\xb7  %d game%s  \xc2\xb7  %d achievement%s, %d points  \xc2\xb7  since %s", hrs, plays_count(&g_plays), plays_count(&g_plays) == 1 ? "" : "s",
             g_ach.n, g_ach.n == 1 ? "" : "s", ach_points(&g_ach), since);
    text(F_BODY, 345, y, C_DIM, st);
    // the buttons
    float bx = 345, by = 312;
    for (int i = 0; i < 4; i++) {
        float bw = text_w(F_SMALL, PP_BTN[i]) + 32;
        int on = g_pp_row == 0 && g_pp_btn == i, dim = i == 3 && g_profs.n < 2;
        if (on) rrect(bx - 3, by - 3, bw + 6, 42, 21, C_ACCENT);
        rrect(bx, by, bw, 36, 18, (rgba){255, 255, 255, (uint8_t)(on ? 255 : 40)});
        text(F_SMALL, bx + 16, by + 6, on ? (rgba){20, 24, 34, 255} : (rgba){255, 255, 255, (uint8_t)(dim ? 110 : 255)}, PP_BTN[i]);
        bx += bw + 14;
    }
    // recently played
    rrect(84, 410, 5, 30, 2, C_ACCENT);
    text(F_TITLE, 104, 402, C_TEXT, "Recently played");
    int rg[12], nr = recent_games(rg, 12);
    if (!nr) text(F_BODY, 104, 470, C_DIM, "Nothing yet: the games you play show here, with the time you played them.");
    const float sz = 178, gap = 28;
    int first = g_pp_row == 1 && g_pp_game > 4 ? g_pp_game - 4 : 0;
    for (int k = first; k < nr; k++) {
        float x = 84 + (k - first) * (sz + gap);
        if (x + sz > g_vw) break;
        int sel = g_pp_row == 1 && g_pp_game == k;
        draw_tile(rg[k], x, 460, sz, sel, 0.8f);
        SDL_Rect clip = {(int)x, 0, (int)sz, (int)g_vh};
        SDL_SetRenderClipRect(g_r, &clip);
        text(F_SMALL, x, 460 + sz + 10, C_TEXT, g_games[rg[k]].title);
        SDL_SetRenderClipRect(g_r, NULL);
        play_t *pl = plays_get(&g_plays, &g_games[rg[k]], 0);
        char h[48];
        hours_text(pl ? pl->secs : 0, h, sizeof h);
        icon(K_CLOCK, x + 10, 460 + sz + 52, 9, C_DIM);
        text(F_SMALL, x + 28, 460 + sz + 40, C_DIM, h);
    }
    float hx = g_vw - 60;
    hint_left(&hx, "ok", "OK", C_TEXT);
    hint_left(&hx, "back", "Back", C_TEXT);
}
// Handles a button on the profile page.
void profile_page_act(int a) {
    int rg[12], nr = recent_games(rg, 12);
    if (a == A_BACK) { screen_open(SCR_DASH); return; }
    if (g_pp_row == 0) {
        if (a == A_LEFT) { moved(g_pp_btn > 0); if (g_pp_btn > 0) g_pp_btn--; }
        else if (a == A_RIGHT) { moved(g_pp_btn < 3); if (g_pp_btn < 3) g_pp_btn++; }
        else if (a == A_DOWN) { moved(nr > 0); if (nr) g_pp_row = 1, g_pp_game = 0; }
        else if (a == A_UP) { g_focus_top = TOP_PROFILE; g_pp_row = -1; moved(1); }
        else if (a == A_OK) {
            if (g_pp_btn == 0) { static const SDL_DialogFileFilter pics[] = {{"Pictures", "png;jpg;jpeg"}}; sound(SND_CONFIRM); SDL_ShowOpenFileDialog(dlg_cb, (void *)3, g_win, pics, 1, NULL, false); }
            else if (g_pp_btn == 1) { snprintf(g_name_buf, sizeof g_name_buf, "%s", g_profs.p[g_profs.cur].name); g_name_mode = 1; popup_open(POP_NAME); }
            else if (g_pp_btn == 2) popup_open(POP_PROFILES);
            else if (g_profs.n > 1) { g_confirm_kind = 1; popup_open(POP_CONFIRM); }
            else sound(SND_BUMP);
        }
    } else if (g_pp_row == 1) {
        if (a == A_LEFT) { moved(g_pp_game > 0); if (g_pp_game > 0) g_pp_game--; }
        else if (a == A_RIGHT) { moved(g_pp_game < nr - 1); if (g_pp_game < nr - 1) g_pp_game++; }
        else if (a == A_UP) { g_pp_row = 0; moved(1); }
        else if (a == A_DOWN) moved(0);
        else if (a == A_OK && g_pp_game < nr) {   // start it from here
            int gi = rg[g_pp_game];
            for (int i = 0; i < g_ngames; i++) if (i == gi) g_sel = i;
            screen_open(SCR_DASH);
            sound(SND_LAUNCH); SDL_Delay(120);
            launch(&g_games[gi]);
        }
    }
}

// ---------- the profiles window and name entry ----------
static const char *const OSK[5] = {"ABCDEFGHIJ", "KLMNOPQRST", "UVWXYZ0123", "456789-_.!", ""};   // the last row: Shift, Space, Delete, Done
// Ends name entry: the profile renamed or made (spaces at the end dropped; an empty name keeps the old one).
void name_done(void) {
    char n[sizeof g_name_buf];
    snprintf(n, sizeof n, "%s", g_name_buf);
    for (char *e = n + strlen(n); e > n && e[-1] == ' ';) *--e = 0;
    if (!n[0]) { sound(SND_BUMP); return; }
    if (g_name_mode == 0) {
        int i = profile_add(&g_profs, n);
        if (i < 0) { toast("Sixteen profiles at most"); popup_close(); return; }
        popup_close();
        profile_use(i);
        screen_open(SCR_PROFILE);   // to give it a picture
    } else {
        snprintf(g_profs.p[g_profs.cur].name, sizeof g_profs.p[0].name, "%s", n);
        profiles_save(&g_profs);
        popup_close();
    }
}
void name_type(const char *t) {   // typed (a keyboard), or a key of the on-screen keyboard
    size_t l = strlen(g_name_buf);
    for (; *t && l < sizeof g_name_buf - 1; t++) {
        char c = *t;
        if ((unsigned char)c < 32) continue;
        if (c >= 'A' && c <= 'Z' && !g_name_shift && l) c = (char)(c - 'A' + 'a');
        g_name_buf[l++] = c, g_name_buf[l] = 0;
    }
    g_name_shift = 0;
}
// Handles a button in a small window (the profiles, name entry, a question).
void popup_act(int a) {
    if (g_popup == POP_PROFILES) {
        int n = g_profs.n + 1;   // the profiles, then New profile
        if (a == A_UP) { moved(g_pop_i > 0); if (g_pop_i > 0) g_pop_i--; }
        else if (a == A_DOWN) { moved(g_pop_i < n - 1); if (g_pop_i < n - 1) g_pop_i++; }
        else if (a == A_BACK) popup_close();
        else if (a == A_OK) {
            if (g_pop_i == g_profs.n) { if (g_profs.n >= PROFILE_MAX) { sound(SND_BUMP); return; } g_name_buf[0] = 0, g_name_mode = 0; g_popup = POP_NONE; popup_open(POP_NAME); }
            else if (g_pop_i == g_profs.cur) { g_popup = POP_NONE; screen_open(SCR_PROFILE); }   // the one in use: its page
            else { profile_use(g_pop_i); popup_close(); sound(SND_CONFIRM); }
        }
        return;
    }
    if (g_popup == POP_NAME) {
        int row = g_name_key / 10, col = g_name_key % 10;
        if (a == A_LEFT) { if (row == 4) { if (col > 0) col--; } else col = (col + 9) % 10; moved(1); }
        else if (a == A_RIGHT) { if (row == 4) { if (col < 3) col++; } else col = (col + 1) % 10; moved(1); }
        else if (a == A_UP) { moved(row > 0); if (row > 0) row--, col = row == 3 && g_name_key / 10 == 4 ? col * 3 : col; }
        else if (a == A_DOWN) { moved(row < 4); if (row < 4) { row++; if (row == 4) col = col / 3 > 3 ? 3 : col / 3; } }
        else if (a == A_BACK) { popup_close(); return; }
        else if (a == A_OK) {
            if (row < 4) { char c[2] = {OSK[row][col], 0}; name_type(c); sound(SND_MOVE); }
            else if (col == 0) { g_name_shift = !g_name_shift; sound(SND_TOGGLE); }
            else if (col == 1) { name_type(" "); sound(SND_MOVE); }
            else if (col == 2) { size_t l = strlen(g_name_buf); if (l) g_name_buf[l - 1] = 0; sound(SND_BACK); }
            else name_done();
        }
        g_name_key = row * 10 + col;
        return;
    }
    if (g_popup == POP_CONFIRM) {
        if (a == A_LEFT) { moved(g_confirm_yes > 0); if (g_confirm_yes > 0) g_confirm_yes--; }
        else if (a == A_RIGHT) { moved(g_confirm_yes < g_confirm_n - 1); if (g_confirm_yes < g_confirm_n - 1) g_confirm_yes++; }
        else if (a == A_BACK || (a == A_OK && !g_confirm_yes)) popup_close();
        else if (a == A_OK) {
            g_popup = POP_NONE;
            if (g_confirm_kind == 6 && g_opt_game >= 0 && g_nbackups) {   // the save now backed up, then the chosen backup put back
                game_t *g = &g_games[g_opt_game];
                char save[1100], which[128];
                snprintf(which, sizeof which, "%s", g_backups[g_backup_i]);
                profile_save_dir(&g_profs, g_profs.cur, g, save, sizeof save);
                if (save_backup(g, save, "before restore", NULL, 0) < 0 || save_restore(g, save, which)) { toast("The backup could not be restored (see the terminal)"); sound(SND_ERROR); return; }
                g_nbackups = save_backups(g, g_backups, 64), g_backup_i = 0;
                LOG("save restored from %s", which);
                toast("Save restored");
            }
            if (g_confirm_kind == 1) {   // the profile goes; its saves stay on disk
                int i = g_profs.cur;
                if (!profile_remove(&g_profs, i)) { if (g_av_tex[i]) SDL_DestroyTexture(g_av_tex[i]); for (int k = i; k < PROFILE_MAX - 1; k++) g_av_tex[k] = g_av_tex[k + 1], g_av_for[k] = g_av_for[k + 1]; g_av_tex[PROFILE_MAX - 1] = NULL; profile_use(0); screen_open(SCR_DASH); }
            } else if (g_confirm_kind == 2) { void album_delete(void); album_delete(); }
            else if (g_confirm_kind == 3 && g_child) {   // the waiting game closes; the chosen one starts when it has
                snprintf(g_pending_launch, sizeof g_pending_launch, "%s", g_games[g_sel].dir);
                game_close();
            }
            sound(SND_CONFIRM);
        }
    }
}
static void draw_panel(float w, float h, const char *title, float *x, float *y) {   // a window in the middle, the rest dimmed
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 0, 0, 0, 150);
    SDL_RenderFillRect(g_r, &all);
    *x = (g_vw - w) / 2, *y = (g_vh - h) / 2;
    rrect(*x, *y, w, h, 18, (rgba){34, 40, 54, 250});
    text(F_TITLE, *x + 32, *y + 22, C_TEXT, title);
}
// Draws the small window open over the screen.
void draw_popup(void) {
    float x, y;
    if (g_popup == POP_PROFILES) {
        int n = g_profs.n + 1;
        float h = 110 + 64.f * n + 50;
        draw_panel(560, h, "Profiles", &x, &y);
        for (int i = 0; i < n; i++) {
            float ry = y + 90 + 64.f * i;
            if (g_pop_i == i) rrect(x + 20, ry - 6, 520, 58, 12, (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 70});
            if (i < g_profs.n) {
                draw_avatar(i, x + 58, ry + 23, 23);
                text(F_BODY, x + 96, ry + 7, C_TEXT, g_profs.p[i].name);
                if (i == g_profs.cur) text(F_SMALL, x + 520 - text_w(F_SMALL, "In use: its page") - 10, ry + 10, C_DIM, "In use: its page");
            } else {
                circle(x + 58, ry + 23, 23, (rgba){255, 255, 255, 50});
                icon(K_PLUS, x + 58, ry + 23, 11, C_TEXT);
                text(F_BODY, x + 96, ry + 7, C_TEXT, "New profile");
            }
        }
        text(F_SMALL, x + 32, y + h - 42, C_DIM, "Each profile has its own saves and play time");
    } else if (g_popup == POP_NAME) {
        draw_panel(700, 470, g_name_mode ? "Rename" : "New profile", &x, &y);
        rrect(x + 32, y + 84, 636, 52, 10, (rgba){255, 255, 255, 30});
        char shown[64];
        snprintf(shown, sizeof shown, "%s%s", g_name_buf, (SDL_GetTicks() / 500) % 2 ? "_" : " ");
        text(F_TITLE, x + 48, y + 88, C_TEXT, shown);
        for (int r = 0; r < 5; r++) {
            int nk = r < 4 ? 10 : 4;
            for (int c = 0; c < nk; c++) {
                float kw = r < 4 ? 56 : 148, kx = x + 32 + c * (r < 4 ? 64 : 160), ky = y + 156 + r * 56;
                int on = g_name_key == r * 10 + c;
                rrect(kx, ky, kw, 48, 10, on ? C_ACCENT : (rgba){255, 255, 255, 36});
                char lab[16];
                if (r < 4) { char ch = OSK[r][c]; if (ch >= 'A' && ch <= 'Z' && !(g_name_shift || !g_name_buf[0])) ch = (char)(ch - 'A' + 'a'); snprintf(lab, sizeof lab, "%c", ch); }
                else snprintf(lab, sizeof lab, "%s", c == 0 ? (g_name_shift ? "SHIFT" : "Shift") : c == 1 ? "Space" : c == 2 ? "Delete" : "Done");
                text(r < 4 && strlen(lab) == 1 ? F_BODY : F_SMALL, kx + (kw - text_w(r < 4 && strlen(lab) == 1 ? F_BODY : F_SMALL, lab)) / 2, ky + 11, on ? (rgba){20, 24, 34, 255} : C_TEXT, lab);
            }
        }
    } else if (g_popup == POP_CONFIRM) {
        char q[300];
        char q2[300] = "";
        const char *title = g_confirm_kind == 1 ? "Delete this profile?" : g_confirm_kind == 3 ? "Close the running game?" : g_confirm_kind == 2 ? "Delete this screenshot?" : "";
        if (g_confirm_kind == 3) snprintf(q, sizeof q, "%s is still running: it closes first, then %s starts", g_play_gi >= 0 ? g_games[g_play_gi].title : "A game", g_games[g_sel].title);
        else if (g_confirm_kind == 1) snprintf(q, sizeof q, "Its play time goes; its saves stay in the game's folder");
        else if (g_confirm_kind == 2) snprintf(q, sizeof q, "The picture file is deleted");
        else if (g_opt_game >= 0) {
            title = "Restore this backup?";
            snprintf(q, sizeof q, "Your save is replaced by %.60s", g_nbackups ? g_backups[g_backup_i] : "");
            snprintf(q2, sizeof q2, "(your save now is backed up first)");
        }
        float pw = g_confirm_n == 3 ? 700 : 560;
        draw_panel(pw, 250, title, &x, &y);
        SDL_Rect qc = {(int)(x + 32), 0, (int)pw - 64, (int)g_vh};
        SDL_SetRenderClipRect(g_r, &qc);
        text(F_SMALL, x + 32, y + 76, C_DIM, q);
        if (q2[0]) text(F_SMALL, x + 32, y + 104, C_DIM, q2);
        SDL_SetRenderClipRect(g_r, NULL);
        for (int i = 0; i < g_confirm_n; i++) {
            float bw = g_confirm_n == 3 ? 190 : 130, gap = g_confirm_n == 3 ? 20 : 30, bx = x + (pw - (g_confirm_n * bw + (g_confirm_n - 1) * gap)) / 2 + i * (bw + gap);
            int on = i == g_confirm_yes;
            rrect(bx, y + 160, bw, 48, 24, on ? C_ACCENT : (rgba){255, 255, 255, 40});
            const char *l = g_confirm_kind == 3 ? (i ? "Close" : "Cancel") : g_confirm_kind == 6 ? (i ? "Restore" : "Cancel") : i ? "Delete" : "Keep";
            text(F_BODY, bx + (bw - text_w(F_BODY, l)) / 2, y + 171, on ? (rgba){20, 24, 34, 255} : C_TEXT, l);
        }
    }
}
