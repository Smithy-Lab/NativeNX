// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, the dashboard: the top bar, the row of games, the background, the hints (nativenx.h lists the parts).
#include "nativenx.h"
// ---------- the dashboard ----------
int g_hover = -1, g_mouse_top;   // the game the pointer is over (not the selected one); the top bar focus came from the pointer
int g_focus_top = -1;   // the selected game; -1, or 0 Settings / 1 Power in the top bar
float g_scroll;                // the row's scroll, eased towards g_sel
int g_prev_bg = -1; float g_bg_fade = 1;
static uint64_t g_last_ms;
int g_bg_built_w;   // the window width the backgrounds were made for
void pics_reset(void) {   // the backgrounds made again (the blur or the window changed)
    SDL_LockMutex(g_bg_mu);
    g_bg_gen++;
    SDL_UnlockMutex(g_bg_mu);
    if (g_pics) for (int i = 0; i < g_ngames; i++) g_pics[i].asked = 0;   // the old one shows until the new one is ready
    g_custom_bg.asked = 0;
    g_bg_built_w = win_px_w();
}
// Scans the games folder again (the selection kept on the same game).
void rescan(void) {
    char pd[1024], ld[1024];
    play_dirs_keep(pd, ld);
    free(g_games);
    SDL_LockMutex(g_bg_mu);
    g_bg_gen++;   // the old games' backgrounds are no longer wanted
    SDL_UnlockMutex(g_bg_mu);
    if (g_pics) for (int i = 0; i < g_ngames; i++) { if (g_pics[i].tex) SDL_DestroyTexture(g_pics[i].tex); if (g_pics[i].bg) SDL_DestroyTexture(g_pics[i].bg); }
    free(g_pics);
    g_ngames = library_scan(g_cfg.games, &g_games);
    g_pics = calloc((size_t)(g_ngames ? g_ngames : 1), sizeof *g_pics);
    if (g_sel >= g_ngames) g_sel = g_ngames ? g_ngames - 1 : 0;
    g_scroll = (float)g_sel;
    g_prev_bg = -1;
    LOG("%d games in %s", g_ngames, g_cfg.games[0] ? g_cfg.games : "(no folder)");
    g_nshow = g_ngames;
    play_dirs_follow(pd, ld);
    order_games(-2);   // the current profile's last played first, and selected
}
// Draws the background: the selected game's (or the chosen picture), cross-fading from the last one.
void draw_background(void) {
    SDL_SetRenderDrawColor(g_r, 18, 22, 34, 255);
    SDL_RenderClear(g_r);
    SDL_Texture *now = NULL, *prev = NULL;
    if (g_cfg.background[0]) { bg_ask(&g_custom_bg, -1, NULL, g_cfg.background); now = g_custom_bg.bg; }
    else if (g_ngames) {
        for (int d = 0; d <= 1; d++)   // the selected one, then its neighbours (ready before they are chosen)
            for (int sg = -d; sg <= d; sg += d ? 2 : 1) {
                int i = g_sel + sg;
                if (i >= 0 && i < g_ngames) bg_ask(&g_pics[i], i, g_games[i].icon, g_games[i].background);
            }
        now = g_pics[g_sel].bg;
        if (g_prev_bg >= 0 && g_prev_bg < g_ngames && g_prev_bg != g_sel) prev = g_pics[g_prev_bg].bg;
        if (!now && prev) now = prev, prev = NULL, g_bg_fade = 0;   // not ready yet: the last one stays, the fade waits
    }
    if (!now && g_ngames && !g_cfg.background[0]) {   // a game without a picture: its colour, dark
        rgba c = title_color(g_games[g_sel].title);
        SDL_Vertex v[4] = {{{0, 0}, fcol((rgba){(uint8_t)(c.r / 2), (uint8_t)(c.g / 2), (uint8_t)(c.b / 2), 255}), {0, 0}}, {{g_vw, 0}, fcol((rgba){(uint8_t)(c.r / 3), (uint8_t)(c.g / 3), (uint8_t)(c.b / 3), 255}), {0, 0}},
                           {{g_vw, g_vh}, fcol((rgba){18, 22, 34, 255}), {0, 0}}, {{0, g_vh}, fcol((rgba){18, 22, 34, 255}), {0, 0}}};
        int idx[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(g_r, NULL, v, 4, idx, 6);
    }
    if (prev && g_bg_fade < 1) draw_cover(prev, 0, 0, g_vw, g_vh, 255);
    if (now) draw_cover(now, 0, 0, g_vw, g_vh, (uint8_t)(255 * (prev ? g_bg_fade : 1)));
    // darker at the left and the bottom, where the text is
    SDL_Vertex v[4] = {{{0, 0}, {0, 0, 0, 0.55f}, {0, 0}}, {{g_vw, 0}, {0, 0, 0, 0.15f}, {0, 0}}, {{g_vw, g_vh}, {0, 0, 0, 0.55f}, {0, 0}}, {{0, g_vh}, {0, 0, 0, 0.8f}, {0, 0}}};
    int idx[6] = {0, 1, 2, 0, 2, 3};
    SDL_RenderGeometry(g_r, NULL, v, 4, idx, 6);
}
// a message over any screen, a few seconds (the game ended badly, nxlinux missing, a profile change held)
void draw_toast(void) {
    if (SDL_GetTicks() >= g_toast_until || !g_toast[0]) return;
    float tw = fminf(g_vw - 120, text_w(F_BODY, g_toast) + 48);
    rrect((g_vw - tw) / 2, g_vh - 150, tw, 52, 14, (rgba){150, 40, 40, 235});
    SDL_Rect clip = {(int)((g_vw - tw) / 2 + 24), 0, (int)(tw - 48), (int)g_vh};
    SDL_SetRenderClipRect(g_r, &clip);
    text(F_BODY, (g_vw - tw) / 2 + 24, g_vh - 138, C_TEXT, g_toast);
    SDL_SetRenderClipRect(g_r, NULL);
}
// the dashboard's block (the title above, the games' row below: 250 high from y 180 at a 720-high window) moved down
// to sit in the middle between the top bar and the hints
float dash_dy(void) { return floorf(g_vh / 2 - 305); }
// Draws the dashboard: the top bar, the row of games, the selected one's title and buttons, the hints, pop-ups.
void draw_dashboard(void) {
    uint64_t ms = SDL_GetTicks();
    float dt = g_last_ms ? (ms - g_last_ms) / 1000.f : 0;
    g_last_ms = ms;
    if (dt > 0.1f) dt = 0.1f;
    g_scroll += (g_sel - g_scroll) * fminf(1, dt * 14);
    if (fabsf(g_sel - g_scroll) < 0.001f) g_scroll = (float)g_sel;
    if (g_bg_fade < 1) g_bg_fade = fminf(1, g_bg_fade + dt * 4);
    float glow = 0.6f + 0.4f * sinf(ms / 400.f);
    draw_background();

    draw_topbar();
    const float dy = dash_dy();   // the games in the middle of the space between the top bar and the hints

    if (!g_ngames) {   // nothing yet: how to start
        icon_folder(140, 300 + dy, 40, (rgba){255, 255, 255, 220});
        text(F_TITLE, 210, 262 + dy, C_TEXT, g_cfg.games[0] ? "No games found here yet" : "Choose your games folder");
        char m[1200];
        snprintf(m, sizeof m, g_cfg.games[0] ? "Each game is a folder with ExeFS and RomFS inside, under %s" : "A folder holding your games, each in a folder with ExeFS and RomFS inside", g_cfg.games);
        text(F_BODY, 210, 318 + dy, C_DIM, m);
        hint_r(210, 372 + dy, "ok", "Choose a folder", C_TEXT);
    } else if (!g_nshow) {   // the favourites only, and there are none
        icon(K_STAR, 140, 300 + dy, 36, (rgba){255, 210, 60, 255});
        text(F_TITLE, 210, 262 + dy, C_TEXT, "No favourites yet");
        char m[200];
        snprintf(m, sizeof m, "%s on a game makes it a favourite; %s shows every game again", btn("fav"), btn("filter"));
        text(F_BODY, 210, 318 + dy, C_DIM, m);
    } else {
        // the selected game large, the others after it
        const float big = 248, small = 160, gap = 14, bx = 84, by = 182 + dy, row_y = by + big - small;   // the row's bottom lines up with the selection's
        // the others by their place relative to the (eased) selection: after it, in a row from x 340; before it, one
        // sliding out on the left
        for (int i = 0; i < g_nshow; i++) {
            if (i == g_sel) continue;
            float pos = i - g_scroll;
            if (pos < -1.5f || pos > 9) continue;
            float x = pos > 0 ? 356 + (pos - 1) * (small + gap) : bx - 22 + pos * (small + gap);
            if (x > g_vw || x + small < 0) continue;
            if (i == g_hover) rrect(x - 4, row_y - 4, small + 8, small + 8, 8, (rgba){255, 255, 255, 200});   // the pointer is over it
            draw_tile(i, x, row_y, small, 0, 0);
        }
        float sx = bx + (g_sel - g_scroll) * (small + gap) * 0.5f;
        draw_tile(g_sel, sx, by, big, g_focus_top < 0, glow);
        const game_t *g = &g_games[g_sel];
        SDL_Rect clip = {345, 0, (int)(g_vw - 345), (int)g_vh};   // in design units: SDL scales a clip rectangle by the render scale itself
        SDL_SetRenderClipRect(g_r, &clip);
        text(F_TITLE, 356, 180 + dy, C_TEXT, g->title);
        SDL_SetRenderClipRect(g_r, NULL);
        float hx = 356;
        hx += hint_r(hx, 228 + dy, "ok", game_waiting(g_sel) ? "Resume" : "Start", C_TEXT);
        hx += hint_r(hx, 228 + dy, "menu", "Options", C_TEXT);
        {   // what is changed for this game: shown by Options, a mark here
            gopts_t o;
            gopts_load(g, &o);
            if (!o.native) { char b[64]; if (o.non) snprintf(b, sizeof b, "Changes on, %d mod%s", o.non, o.non == 1 ? "" : "s"); else snprintf(b, sizeof b, "Changes on"); float bw = text_w(F_SMALL, b) + 20; rrect(hx, 230 + dy, bw, 28, 14, (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 70}); text(F_SMALL, hx + 10, 233 + dy, C_TEXT, b); hx += bw + 16; }
        }
        if (g->publisher[0]) { text(F_SMALL, hx, 232 + dy, C_DIM, g->publisher); hx += text_w(F_SMALL, g->publisher) + 16; }
        if (g->version[0]) { char v[48]; snprintf(v, sizeof v, "v%s", g->version); text(F_SMALL, hx, 232 + dy, C_DIM, v); hx += text_w(F_SMALL, v) + 16; }
        if (g->ndlc) { char b[32]; snprintf(b, sizeof b, "%d DLC", g->ndlc); text(F_SMALL, hx, 232 + dy, C_DIM, b); hx += text_w(F_SMALL, b) + 16; }   // its DLC
        if (g->gfx != GFX_UNKNOWN) {   // what it draws with: NVN (the Switch's own), or OpenGL / Vulkan on the PC's driver
            const char *b = gfx_api_name(g->gfx);
            float bw = text_w(F_SMALL, b) + 20;
            rrect(hx, 230 + dy, bw, 28, 14, g->gfx == GFX_NVN ? (rgba){255, 255, 255, 28} : (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 70});
            text(F_SMALL, hx + 10, 233 + dy, g->gfx == GFX_NVN ? C_DIM : C_TEXT, b);
            hx += bw + 16;
        }
        {   // its achievements: how many unlocked, of how many
            achg_t *ag = ach_game(g_sel);
            if (ag && ag->set.n) {
                char b[48];
                snprintf(b, sizeof b, "%d / %d", ach_game_count(&g_ach, ach_pid(g)), ag->set.n);
                icon(K_TROPHY, hx + 10, 245 + dy, 10, (rgba){255, 200, 60, 255});
                text(F_SMALL, hx + 26, 232 + dy, C_DIM, b);
            }
        }
    }
    // bottom: the controller, and what the buttons do
    draw_pad_badge(60, g_vh - 78);
    float hx = g_vw - 60;
    hint_left(&hx, "view", "Settings", C_DIM);
    hint_left(&hx, "prof", "Profiles", C_DIM);
    if (g_ngames) {
        hint_left(&hx, "filter", g_fav_only ? "All games" : "Favourites", C_DIM);
        if (g_nshow) hint_left(&hx, "fav", is_fav(g_sel) ? "Unfavourite" : "Favourite", C_DIM);
        if (g_nshow) hint_left(&hx, "ach", "Achievements", C_DIM);
    }
}
