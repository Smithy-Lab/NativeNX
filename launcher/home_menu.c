// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, the menu over a running game (the Home button): drawn here, shown in the game's window (nativenx.h lists the parts).
#include "nativenx.h"
static const char *const OV_LABEL[OV_N] = {"Resume", "Achievements", "Take a screenshot", "NativeNX Home", "Close the game"};
int g_ov_open, g_ov_sel, g_ov_list, g_ov_row, g_ov_top, g_ov_w, g_ov_h, g_ov_close_armed, g_ov_seq, g_ov_nin;
static uint64_t g_ov_note_until;
static char g_ov_note[80];
static SDL_Texture *g_ov_target;
static char g_gline[512];
static size_t g_gline_n;
static void game_say(const char *line) {
    if (g_ach_fd < 0) return;
    char b[1300];
    int n = snprintf(b, sizeof b, "%s\n", line);
    if (write(g_ach_fd, b, (size_t)n) != n) LOG("the game's window did not take '%s'", line);
}
void ov_game_ended(void) {
    g_ov_open = 0, g_suspended = 0, g_gline_n = 0;
    if (g_ov_target) SDL_DestroyTexture(g_ov_target), g_ov_target = NULL;
}
// Draws the menu over the running game (the game, its time this session, the choices, its achievements).
static void draw_overlay(void) {
    SDL_FRect all = {0, 0, g_vw, g_vh};
    SDL_SetRenderDrawColor(g_r, 6, 8, 14, 165);
    SDL_RenderFillRect(g_r, &all);
    int gi = g_play_gi;
    if (gi < 0 || gi >= g_ngames) return;
    const game_t *g = &g_games[gi];
    // the left panel: the game, the choices
    const float px = 40, py = 40, pw = 420, ph = g_vh - 80;
    rrect(px, py, pw, ph, 22, (rgba){22, 25, 34, 240});
    draw_tile(gi, px + 24, py + 24, 96, 0, 0);
    SDL_Rect clip = {(int)(px + 136), 0, (int)(pw - 152), (int)g_vh};
    SDL_SetRenderClipRect(g_r, &clip);
    text(F_BODYB, px + 136, py + 28, C_TEXT, g->title);
    SDL_SetRenderClipRect(g_r, NULL);
    char t[96], h[48];
    hours_text((long long)(play_ms() / 1000), h, sizeof h);
    snprintf(t, sizeof t, "Playing for %s", h);
    text(F_SMALL, px + 136, py + 62, C_DIM, t);
    achg_t *a = ach_game(gi);
    uint64_t pid = ach_pid(g);
    int got = 0, pts = 0;
    if (a) for (int i = 0; i < a->set.n; i++) if (ach_unlocked(&g_ach, pid, a->set.a[i].id)) got++, pts += a->set.a[i].points;
    if (a && a->set.n) {
        snprintf(t, sizeof t, "%d / %d  \xc2\xb7  %d points", got, a->set.n, pts);
        icon(K_TROPHY, px + 146, py + 102, 10, (rgba){255, 200, 60, 255});
        text(F_SMALL, px + 162, py + 89, C_DIM, t);
        rrect(px + 24, py + 140, pw - 48, 8, 4, (rgba){255, 255, 255, 40});
        if (a->set.total_points) rrect(px + 24, py + 140, fmaxf(8, (pw - 48) * pts / a->set.total_points), 8, 4, C_ACCENT);
    }
    for (int i = 0; i < OV_N; i++) {
        float by = py + 176 + i * 62;
        int on = i == g_ov_sel && !g_ov_list;
        if (on) rrect(px + 16, by, pw - 32, 52, 14, C_ACCENT);
        else if (i == g_ov_sel) rrect(px + 16, by, pw - 32, 52, 14, (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 60});
        const char *l = i == OV_CLOSE && g_ov_close_armed ? "Press again to close the game" : OV_LABEL[i];
        text(F_BODY, px + 40, by + 12, on ? (rgba){20, 24, 34, 255} : (i == OV_CLOSE ? (rgba){255, 140, 130, 255} : C_TEXT), l);
    }
    float hx = px + 24, hy = py + ph - 52;
    hx += hint_r(hx, hy, "ok", "Select", C_TEXT);
    hint_r(hx, hy, "back", "Resume", C_TEXT);
    // the right: the achievements (the list when it has the focus)
    float rx = px + pw + 40, rw = g_vw - rx - 40;
    if (rw > 360 && a && a->set.n) {
        text(F_TITLE, rx, py + 70, C_TEXT, "Achievements");
        ach_page_order(a, pid);
        int nshow = (int)((g_vh - 80 - (py + 140)) / AV_ROW);
        if (nshow < 1) nshow = 1;
        draw_ach_rows(a, pid, rx, py + 140, rw, nshow, g_ov_list ? g_ov_row : -1, &g_ov_top);
    }
    draw_status(g_vw - 60, 46);
    if (SDL_GetTicks() < g_ov_note_until) {   // "Screenshot saved"
        float tw = text_w(F_BODY, g_ov_note) + 48;
        rrect((g_vw - tw) / 2, g_vh - 110, tw, 52, 14, (rgba){30, 34, 46, 240});
        text(F_BODY, (g_vw - tw) / 2 + 24, g_vh - 98, C_TEXT, g_ov_note);
    }
}
// the menu drawn at the game window's shape (720 design units high, at most 1080 pixels), sent to the game
static void ov_send(void) {
    if (!g_ov_open || !g_r) return;
    int h = g_ov_h > 1080 ? 1080 : g_ov_h < 360 ? 360 : g_ov_h, w = (int)((int64_t)g_ov_w * h / (g_ov_h ? g_ov_h : 1));
    if (w < 480) w = 480;
    float tw = 0, th = 0;
    if (g_ov_target) SDL_GetTextureSize(g_ov_target, &tw, &th);
    if (!g_ov_target || (int)tw != w || (int)th != h) {
        if (g_ov_target) SDL_DestroyTexture(g_ov_target);
        g_ov_target = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!g_ov_target) { LOG("the menu: no picture to draw into (%s)", SDL_GetError()); return; }
        SDL_SetTextureBlendMode(g_ov_target, SDL_BLENDMODE_BLEND);
    }
    SDL_SetRenderTarget(g_r, g_ov_target);
    layout();
    SDL_SetRenderScale(g_r, 1, 1);
    SDL_SetRenderDrawColor(g_r, 0, 0, 0, 0);
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_NONE);
    SDL_RenderClear(g_r);
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderScale(g_r, g_s, g_s);
    draw_overlay();
    SDL_SetRenderScale(g_r, 1, 1);
    SDL_Surface *sf = SDL_RenderReadPixels(g_r, NULL);
    SDL_SetRenderTarget(g_r, NULL);   // (the window's own scale comes back when it draws again: layout(), at Home. Not
                                      // here: the fonts are made again at each change of scale, twice a button press)
    if (!sf) { LOG("the menu could not be read back: %s", SDL_GetError()); return; }
    SDL_Surface *c = SDL_ConvertSurface(sf, SDL_PIXELFORMAT_ABGR8888);   // RGBA bytes
    SDL_DestroySurface(sf);
    if (!c) return;
    char path[1100];
    snprintf(path, sizeof path, "%s/nxlinux-menu-%d-%d.rgba", nx_runtime_dir(), (int)getpid(), g_ov_seq++ % 4);
    FILE *f = fopen(path, "wb");
    if (f) {   // NXLIMG02: premultiplied (drawn over transparent with SDL's blending, which keeps it so)
        uint32_t wh[2] = {(uint32_t)c->w, (uint32_t)c->h};
        fwrite("NXLIMG02", 1, 8, f), fwrite(wh, 4, 2, f);
        for (int y = 0; y < c->h; y++) fwrite((uint8_t *)c->pixels + (size_t)y * c->pitch, 4, (size_t)c->w, f);
        fclose(f);
        char line[1200];
        snprintf(line, sizeof line, "menu %s", path);
        game_say(line);
    }
    SDL_DestroySurface(c);
}
static void ov_close(void) {
    if (!g_ov_open) return;
    g_ov_open = 0;
    game_say("menu-off");
    sound(SND_CLOSE);
}
void game_resume(void) {   // from NativeNX's Home back into the game
    if (!g_child || !g_suspended) return;
    nxl_proc_signal(g_child, NXL_PROC_CONT);
    g_suspended = 0, g_play_t0 = SDL_GetTicks();
    game_say("show");
    if (g_win) SDL_HideWindow(g_win);
    LOG("back to %s", g_play_gi >= 0 ? g_games[g_play_gi].title : "the game");
}
static void game_to_home(void) {   // the game waits: stopped, its window out of the way; NativeNX in front
    if (!g_child || g_suspended) return;
    ov_close();
    game_say("hide");
    SDL_Delay(250);   // the window minimised before the process stops
    nxl_proc_signal(g_child, NXL_PROC_STOP);
    g_suspended = 1;
    g_play_acc = play_ms(), g_play_t0 = 0;
    for (int i = 0; i < g_nshow; i++) if (i == g_play_gi) g_sel = i, g_scroll = (float)i;
    screen_open(SCR_DASH);
    if (g_win) SDL_ShowWindow(g_win), SDL_RaiseWindow(g_win);
    LOG("%s waits at NativeNX's Home", g_play_gi >= 0 ? g_games[g_play_gi].title : "the game");
}
void game_close(void) {   // the game ended from the menu (or to start another): as closing its window
    if (!g_child) return;
    if (g_suspended) nxl_proc_signal(g_child, NXL_PROC_CONT), g_suspended = 0;
    nxl_proc_signal(g_child, NXL_PROC_TERM);
    LOG("closing %s", g_play_gi >= 0 ? g_games[g_play_gi].title : "the game");
}
// Handles a button in the menu over the game.
static void ov_act(int a) {
    if (g_ov_list) {   // in the achievements list
        achg_t *ag = ach_game(g_play_gi);
        int n = ag ? ag->set.n : 0;
        if (a == A_UP) { moved(g_ov_row > 0); if (g_ov_row > 0) g_ov_row--; }
        else if (a == A_DOWN) { moved(g_ov_row < n - 1); if (g_ov_row < n - 1) g_ov_row++; }
        else if (a == A_BACK || a == A_LEFT) { g_ov_list = 0; sound(SND_BACK); }
        return;
    }
    if (a != A_OK && g_ov_sel == OV_CLOSE) g_ov_close_armed = 0;
    if (a == A_UP) { moved(g_ov_sel > 0); if (g_ov_sel > 0) g_ov_sel--; }
    else if (a == A_DOWN) { moved(g_ov_sel < OV_N - 1); if (g_ov_sel < OV_N - 1) g_ov_sel++; }
    else if (a == A_RIGHT && g_ov_sel == OV_ACH) { g_ov_list = 1, g_ov_row = 0; sound(SND_MOVE); }
    else if (a == A_BACK) ov_close();
    else if (a == A_OK) {
        switch (g_ov_sel) {
        case OV_RESUME: ov_close(); break;
        case OV_ACH: g_ov_list = 1, g_ov_row = 0; sound(SND_CONFIRM); break;
        case OV_SHOT: game_say("shot"); snprintf(g_ov_note, sizeof g_ov_note, "Screenshot saved to the Album"); g_ov_note_until = SDL_GetTicks() + 2500; sound(SND_CONFIRM); break;
        case OV_HOME: sound(SND_CONFIRM); game_to_home(); break;
        case OV_CLOSE:
            if (!g_ov_close_armed) { g_ov_close_armed = 1; sound(SND_BUMP); break; }
            g_ov_close_armed = 0;
            ov_close();
            game_close();
            break;
        }
    }
}
// the game's lines: "open W H" (the Home button), "key NAME" (a button while the menu is up), "pad nintendo"
static void game_line(const char *l) {
    int w, h;
    if (sscanf(l, "open %d %d", &w, &h) == 2) {
        g_ov_open = 1, g_ov_sel = 0, g_ov_list = 0, g_ov_close_armed = 0, g_ov_top = 0;
        g_ov_w = w, g_ov_h = h;
        if (g_play_gi >= 0) ach_check(g_play_gi, 1);
        sound(SND_OPEN);
        LOG("the menu over the game (%dx%d)", w, h);
        return;
    }
    if (!strcmp(l, "pad nintendo")) { g_ov_nin = 1; return; }
    if (strncmp(l, "key ", 4) || !g_ov_open) return;
    const char *k = l + 4;
    int ok_east = g_ov_nin || g_confirm_east;   // A where the controller has it (Nintendo's: the right-hand button)
    int a = !strcmp(k, "up") ? A_UP : !strcmp(k, "down") ? A_DOWN : !strcmp(k, "left") ? A_LEFT : !strcmp(k, "right") ? A_RIGHT
          : !strcmp(k, "enter") ? A_OK : !strcmp(k, "esc") || !strcmp(k, "home") || !strcmp(k, "start") ? A_BACK
          : !strcmp(k, "south") ? (ok_east ? A_BACK : A_OK) : !strcmp(k, "east") ? (ok_east ? A_OK : A_BACK) : -1;
    if (!strcmp(k, "home") || !strcmp(k, "start")) { ov_close(); return; }
    if (a >= 0) ov_act(a);
}
// Reads what the running game's window sends (menu wanted, buttons while it is up, the game waiting at Home).
void game_poll(void) {
    if (g_game_fd < 0) return;
    int was = g_ov_open, n = 0;
    for (;;) {
        ssize_t k = read(g_game_fd, g_gline + g_gline_n, sizeof g_gline - 1 - g_gline_n);
        if (k <= 0) break;
        g_gline_n += (size_t)k;
        char *nl;
        while ((nl = memchr(g_gline, '\n', g_gline_n))) {
            *nl = 0;
            game_line(g_gline);
            n++;
            size_t used = (size_t)(nl - g_gline) + 1;
            memmove(g_gline, g_gline + used, g_gline_n - used);
            g_gline_n -= used;
        }
        if (g_gline_n == sizeof g_gline - 1) g_gline_n = 0;
    }
    if (g_ov_open && (n || !was)) ov_send();   // something changed: the menu again
    else if (g_ov_open && g_ov_note_until && SDL_GetTicks() > g_ov_note_until) { g_ov_note_until = 0; ov_send(); }
}
