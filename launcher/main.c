// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX (nativenx): a console-style dashboard for the games in a folder (library.h), driven by a controller, the
// keyboard or the mouse, which starts nxlinux on the chosen game and comes back when it ends.
//   the top bar: a profile badge, Settings and Power, the clock and (on a machine with one) the battery
//   the row: the selected game's picture large, its title and the buttons that act on it, the other games after it
//   the background: the game's background picture, or its picture blurred (or the picture chosen in Settings)
//   bottom left: the controller in use; bottom right: what the buttons do
// Usage: nativenx [--games DIR] [--install] [--fullscreen] [--list] [--screenshot FILE.png]
#include "nativenx.h"
#include <SDL3/SDL_main.h>   // (on Windows: main as the program's entry point, its arguments in UTF-8)

SDL_Window *g_win;
SDL_Renderer *g_r;
float g_s = 1;            // design units -> pixels
float g_vw = DW, g_vh = DH;   // the window in design units
config_t g_cfg;
game_t *g_games;
int g_ngames;
int g_sel;          // the selected game
int g_nshow;        // the games the row shows: all, or the favourites first (g_fav_only)
int g_fav_only;     // the row shows the favourites only
int g_screen, g_popup, g_pop_i, g_news_top;
profiles_t g_profs; // who plays (profile.c), and the current one's time per game
plays_t g_plays;
ach_store_t g_ach;  // the current profile's achievements (achieve.c)
const char *const TOP_LABEL[TOP_N] = {"Profile", "What's new", "Album", "Controllers", "Settings", "Exit"};
float top_x(int i) { return i == TOP_PROFILE ? 70 : 162 + 60 * (i - 1); }
int is_fav(int gi) { play_t *p = gi >= 0 && gi < g_ngames ? plays_get(&g_plays, &g_games[gi], 0) : NULL; return p && p->fav; }

// ---------- main ----------
int win_px_w(void) { int w = 1280, h; if (g_r) SDL_GetCurrentRenderOutputSize(g_r, &w, &h); return w; }
// Fits the layout to the window's size (its scale, the fonts and backgrounds made again when it changed a lot).
void layout(void) {
    int w, h;
    SDL_GetCurrentRenderOutputSize(g_r, &w, &h);
    if (g_bg_built_w && (w > g_bg_built_w * 5 / 4 || w < g_bg_built_w * 3 / 4)) pics_reset();   // much bigger or smaller: made again for it
    if (!g_bg_built_w) g_bg_built_w = w;
    g_s = fminf(w / (float)DW, h / (float)DH);
    if (g_s <= 0) g_s = 1;
    g_vw = w / g_s, g_vh = h / g_s;
    SDL_SetRenderScale(g_r, g_s, g_s);
    fonts_for_scale();
}
// Writes what the window shows to a PNG (--screenshot, for the tests and the docs); 0 on success.
int screenshot(const char *path) {
    SDL_SetRenderScale(g_r, 1, 1);
    SDL_Surface *s = SDL_RenderReadPixels(g_r, NULL);
    SDL_SetRenderScale(g_r, g_s, g_s);
    if (!s) { LOG("cannot read the window: %s", SDL_GetError()); return 1; }
    SDL_Surface *c = SDL_ConvertSurface(s, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(s);
    int ok = c && stbi_write_png(path, c->w, c->h, 4, c->pixels, c->pitch);
    if (c) SDL_DestroySurface(c);
    LOG("%s %s", ok ? "picture written to" : "could not write", path);
    return !ok;
}
// ---------- the command line ----------
// what it asks for; most of it is for the tests and the pictures in the docs (a screen, a selection, presses)
typedef struct {
    const char *shot, *screen, *select_title, *press;
    int fullscreen, list, select, win_w, win_h, run, ach, options_at, loading, click;
    float hover_x, hover_y;
} args_t;
static int usage(const char *me) {
    fprintf(stderr, "usage: %s [--games DIR] [--fullscreen] [--install] [--list] [--run N] [--screenshot FILE.png [--select N] [--settings N] [--options N] [--size WxH]]\n", me);
    return 2;
}
// Reads the command line into a; -1 to go on, else the exit code of a command that is done (--install, --export-sounds)
static int args_parse(int argc, char **argv, args_t *a) {
    *a = (args_t){.fullscreen = getenv("SteamDeck") != NULL, .win_w = DW, .win_h = DH, .hover_x = -1, .hover_y = -1};
    for (int i = 1; i < argc; i++) {
        const char *o = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(o, "--fullscreen")) a->fullscreen = 1;
        else if (!strcmp(o, "--list")) a->list = 1;
        else if (!strcmp(o, "--splash")) g_splash_on = g_splash_hold = 1;          // pictures: the logo shown at start
        else if (!strcmp(o, "--install")) { data_find(); return desktop_install(); }   // add NativeNX to the app menu
        else if (!v) return usage(argv[0]);
        else {   // the options with a value
            i++;
            if (!strcmp(o, "--games")) { snprintf(g_cfg.games, sizeof g_cfg.games, "%s", v); config_save(&g_cfg); }
            else if (!strcmp(o, "--export-sounds")) return export_sounds(v);
            else if (!strcmp(o, "--screenshot")) a->shot = v;
            else if (!strcmp(o, "--select")) a->select = atoi(v);
            else if (!strcmp(o, "--select-title")) a->select_title = v;   // pictures: the game whose title has this in it
            else if (!strcmp(o, "--loading")) a->loading = atoi(v);          // pictures: game N's loading screen, a minute in
            else if (!strcmp(o, "--screen")) a->screen = v;                  // pictures: profile, profiles, name, album, news, achievements
            else if (!strcmp(o, "--settings")) {   // open on settings category c (10 x (row + 1) + c: on a row)
                int c = atoi(v);
                g_settings = 1, g_set_cat = c % 10, g_set_col = c >= 10, g_set_row = c / 10 > 0 ? c / 10 - 1 : 0;
            }
            else if (!strcmp(o, "--options")) a->options_at = atoi(v) + 1;  // open on the selected game's options, category N (as --settings)
            else if (!strcmp(o, "--pad")) g_fake_pad = v;                     // pictures: show this controller kind
            else if (!strcmp(o, "--hover")) sscanf(v, "%f,%f", &a->hover_x, &a->hover_y);   // pictures: the pointer at (x, y), design units
            else if (!strcmp(o, "--click")) { sscanf(v, "%f,%f", &a->hover_x, &a->hover_y); a->click = 1; }   // ... and a left click there
            else if (!strcmp(o, "--press")) a->press = v;                     // buttons pressed first (l r u d a b m v y x g t)
            else if (!strcmp(o, "--size")) sscanf(v, "%dx%d", &a->win_w, &a->win_h);
            else if (!strcmp(o, "--achievements")) a->ach = atoi(v);          // game N's achievements against its saves, no window
            else if (!strcmp(o, "--run")) a->run = atoi(v);                   // start game N (as --list numbers them, from 1), no window
            else return usage(argv[0]);
        }
    }
    return -1;
}

// ---------- the commands without a window ----------
// --list: what NativeNX sees in the games folder
static int cmd_list(void) {
    game_t *gs;
    int n = library_scan(g_cfg.games, &gs);
    printf("%d games in %s\n", n, g_cfg.games[0] ? g_cfg.games : "(no folder: --games DIR)");
    for (int i = 0; i < n; i++) {
        printf("%d. %s\n  folder %s\n  program %016llx, title from %s, picture %s\n", i + 1, gs[i].title, gs[i].dir, (unsigned long long)gs[i].program_id,
               gs[i].title_from_nacp ? "control.nacp" : "the folder name", gs[i].icon[0] ? gs[i].icon : "(none: cover.png / cover.jpg in the folder)");
        if (gs[i].ndlc) printf("  DLC: %d in %s\n", gs[i].ndlc, gs[i].dlc);
        printf("  graphics: %s%s\n", gfx_api_name(gs[i].gfx), gs[i].gfx == GFX_OPENGL || gs[i].gfx == GFX_VULKAN ? " (its calls go to your PC's driver)" : "");
        if (gs[i].fstep) printf("  60 fps: available (nxnative's frame step knows this build)\n");
        for (int k = 0; k < gs[i].nenv; k++) printf("  %s\n", gs[i].env[k]);
    }
    free(gs);
    return 0;
}
// --achievements N: each of game N's achievements against the current profile's saves (names and numbers only)
static int cmd_achievements(int n) {
    g_ngames = library_scan(g_cfg.games, &g_games);
    if (n < 1 || n > g_ngames) { fprintf(stderr, "no game %d (there are %d)\n", n, g_ngames); return 2; }
    data_find();
    int gi = n - 1;
        achg_t *a = ach_game(gi);
    char save[1100];
    profile_save_dir(&g_profs, g_profs.cur, &g_games[gi], save, sizeof save);
    ach_saves_read(&a->set, save, &a->sv);
    ach_ctx_t c;
    ach_ctx_for(gi, a, &c);
    ach_eval(&a->set, &c, a->st);
    printf("%s (program %016llx), profile %s\n", g_games[gi].title, (unsigned long long)g_games[gi].program_id, g_profs.p[g_profs.cur].name);
    printf("list: %s; save folder %s: %d save slot%s with %s\n", a->set.fmt ? "the game's own and the general ones" : "the general ones only", save, a->sv.n, a->sv.n == 1 ? "" : "s", a->set.save_file[0] ? a->set.save_file : "-");
    printf("played %.1f hours, %d screenshots, favourite %s\n", c.play_hours, c.screenshots, c.favourite ? "yes" : "no");
    int met = 0, untracked = 0;
    for (int i = 0; i < a->set.n; i++) {
        const ach_state_t *st = &a->st[i];
        long long u = ach_unlocked(&g_ach, ach_pid(&g_games[gi]), a->set.a[i].id);
        const char *mark = u ? "unlocked" : st->state == 1 ? "met" : st->state == 0 ? "not yet" : st->state == -1 ? "NOT TRACKED" : "no save";
        met += st->state == 1, untracked += st->state == -1;
        printf("  %-11s %3d  %-34s", mark, a->set.a[i].points, a->set.a[i].name);
        if (st->target > 1) printf("  %.0f / %.0f", st->value, st->target);
        printf("\n");
        if (st->state == -1) printf("              condition: %s\n", a->set.a[i].when);
    }
    printf("%d of %d met; %d not tracked (a flag the save does not have: its name is wrong, or this version lacks it)\n", met, a->set.n, untracked);
    return 0;
}
// --run N: starts game N and waits for it to end
static int cmd_run(int n) {
    g_ngames = library_scan(g_cfg.games, &g_games);
    if (n < 1 || n > g_ngames) { fprintf(stderr, "no game %d (there are %d)\n", n, g_ngames); return 2; }
    launch(&g_games[n - 1]);
    if (!g_child) { fprintf(stderr, "%s\n", g_toast); return 1; }
    while (g_child) { child_poll(); usleep(20000); }
    printf("game ended: %s %d, log %s\n", g_last_status >= 0 ? "exit status" : "signal", g_last_status >= 0 ? g_last_status : -g_last_status, g_log);
    return 0;
}

// ---------- the window ----------
// SDL, the fonts, the window and its renderer, the launcher's files and sounds; 0 when it is up
static int window_start(const args_t *a) {
#ifndef _WIN32
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);   // the game's window gone: a pop-up written to its pipe fails, it does not end the launcher
#endif
    SDL_SetAppMetadata("NativeNX", NXL_VERSION, "nativenx");   // the app id: the desktop matches the window to nativenx.desktop and its icon
    SDL_SetHint(SDL_HINT_APP_ID, "nativenx");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) { LOG("SDL_Init: %s", SDL_GetError()); return -1; }
    data_find();   // (first: the fonts' last resort is in it)
    g_ttf = find_font(0), g_ttf_bold = find_font(1);
    if (!g_ttf) { LOG("no TrueType font found (install Liberation Sans or DejaVu Sans)"); return -1; }
    stbtt_InitFont(&g_fi, g_ttf, stbtt_GetFontOffsetForIndex(g_ttf, 0));
    if (g_ttf_bold) stbtt_InitFont(&g_fi_bold, g_ttf_bold, stbtt_GetFontOffsetForIndex(g_ttf_bold, 0));
    g_win = SDL_CreateWindow("NativeNX", a->win_w, a->win_h, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (a->fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
    if (!g_win) { LOG("SDL_CreateWindow: %s", SDL_GetError()); return -1; }
    g_r = SDL_CreateRenderer(g_win, NULL);
    if (!g_r) { LOG("SDL_CreateRenderer: %s", SDL_GetError()); return -1; }
    SDL_SetRenderVSync(g_r, 1);
    SDL_SetRenderDrawBlendMode(g_r, SDL_BLENDMODE_BLEND);
    g_dlg_mu = SDL_CreateMutex();
    window_icon();
    sound_init();
    bg_init();
    return 0;
}
// The screen the window opens on: the dashboard, or what the command line asks for (pictures, tests)
static void first_screen(args_t *a) {
    if (!a->shot && !a->press && a->hover_x < 0) g_splash_on = 1, g_splash_t0 = SDL_GetTicks();
    rescan();
    g_still = a->shot != NULL;
    if (a->select_title) for (int i = 0; i < g_nshow; i++) if (strstr(g_games[i].title, a->select_title)) { a->select = i; break; }
    if (a->select > 0 && a->select < g_ngames) g_sel = a->select, g_scroll = (float)a->select;
    if (a->screen) {
        const char *s = a->screen;
        g_splash_on = 0;
        if (!strcmp(s, "profile")) g_screen = SCR_PROFILE;
        else if (!strcmp(s, "album")) g_screen = SCR_ALBUM, album_scan();
        else if (!strcmp(s, "news")) g_screen = SCR_NEWS;
        else if (!strcmp(s, "achievements")) ach_page_open(g_sel);
        else if (!strcmp(s, "profiles")) g_popup = POP_PROFILES, g_pop_i = g_profs.n;
        else if (!strcmp(s, "name")) g_popup = POP_NAME, g_name_mode = 0, snprintf(g_name_buf, sizeof g_name_buf, "Sam"), g_name_key = 12;
    }
    if (a->loading > 0 && a->loading <= g_ngames) {
        g_loading = 1, g_load_gi = g_sel = a->loading - 1, g_load_stage = 2, g_load_t0 = SDL_GetTicks() - 75000, g_splash_on = 0;
        snprintf(g_load_last, sizeof g_load_last, "41489 entry points (237006 more from function addresses in data)");
    }
    if (a->options_at && g_ngames) {
        int c = a->options_at - 1;
        settings_open(2);
        g_set_cat = c % 10, g_set_col = c >= 10, g_set_row = c / 10 > 0 ? c / 10 - 1 : 0;
    }
}
// The buttons --press names and the pointer --hover / --click puts down, as a player would (on the first frame)
static void scripted_input(const args_t *a) {
    if (a->press) {
        for (const char *c = a->press; *c; c++) {
            int k = *c == 'l' ? A_LEFT : *c == 'r' ? A_RIGHT : *c == 'u' ? A_UP : *c == 'd' ? A_DOWN : *c == 'a' ? A_OK : *c == 'b' ? A_BACK : *c == 'm' ? A_MENU : *c == 'v' ? A_VIEW
                  : *c == 'y' ? A_FAV : *c == 'x' ? A_PROF : *c == 'g' ? A_FILTER : *c == 't' ? A_ACH : -1;
            if (k >= 0) act(k);
        }
        LOG("after the presses: game %d, top bar %d, settings %d (category %d, column %d, row %d), screen %d, window %d, showing %d, selected '%s', profile '%s'", g_sel, g_focus_top, g_settings, g_set_cat, g_set_col, g_set_row,
            g_screen, g_popup, g_nshow, g_ngames ? g_games[g_sel].title : "", g_profs.p[g_profs.cur].name);
    }
    if (a->hover_x >= 0) {
        SDL_Event ev = {0};
        ev.type = SDL_EVENT_MOUSE_MOTION, ev.motion.x = a->hover_x * g_s, ev.motion.y = a->hover_y * g_s;
        on_event(&ev);
        if (a->click) {
            SDL_Event c = {0};
            c.type = SDL_EVENT_MOUSE_BUTTON_DOWN, c.button.button = SDL_BUTTON_LEFT, c.button.x = a->hover_x * g_s, c.button.y = a->hover_y * g_s;
            on_event(&c);
        }
        LOG("pointer at %.0f,%.0f%s: hovering game %d, top bar %d, settings column %d row %d, screen %d, window %d (row %d, key %d), page row %d button %d",
            a->hover_x, a->hover_y, a->click ? " (clicked)" : "", g_hover, g_focus_top, g_set_col, g_set_row, g_screen, g_popup, g_pop_i, g_name_key, g_pp_row, g_pp_btn);
    }
}
// Draws the screen showing (and what is over it)
static void draw_frame(void) {
    if (g_settings) draw_settings();
    else if (g_screen == SCR_PROFILE) draw_profile_page();
    else if (g_screen == SCR_ALBUM) draw_album();
    else if (g_screen == SCR_NEWS) draw_news();
    else if (g_screen == SCR_ACH) draw_ach_page();
    else draw_dashboard();
    if (g_popup) draw_popup();
    draw_toast();
    draw_ach_popups();
    draw_splash();
}
// The main loop, until Exit: while a game runs NativeNX idles (quick to answer its menu); while one starts, the
// loading screen; else the screens drawn every frame. With --screenshot: the picture written after 30 frames.
static int main_loop(const args_t *a) {
    int frames = 0;
    while (!g_quit) {
        SDL_Event e;
        if (g_child && !g_loading && !g_suspended) {   // the game runs
            if (SDL_WaitEventTimeout(&e, g_ov_open ? 8 : 40)) do on_event(&e); while (SDL_PollEvent(&e));
            child_poll(); ach_tick(); game_poll();
            continue;
        }
        if (g_loading) {   // the game is starting: the loading screen, about 30 times a second
            while (SDL_PollEvent(&e)) on_event(&e);
            child_poll();
            loading_poll();
            layout();
            if (g_loading) {
                draw_loading();
                if (a->shot && ++frames >= 30) return screenshot(a->shot);
                SDL_RenderPresent(g_r);
                SDL_Delay(25);
            }
            continue;
        }
        while (SDL_PollEvent(&e)) on_event(&e);
        hold_tick();
        child_poll();
        ach_tick();
        dlg_poll();
        bg_collect();
        layout();
        if (frames == 1) scripted_input(a);
        draw_frame();
        if (a->shot && ++frames >= 30) {   // a picture, once the backgrounds asked for are made (at most 5 s more)
            SDL_LockMutex(g_bg_mu);
            int busy = g_nbgq;
            SDL_UnlockMutex(g_bg_mu);
            if (!busy || frames > 330) return screenshot(a->shot);
        }
        if (!a->shot) frames = frames < 2 ? frames + 1 : frames;   // (the scripted input runs once, on frame 1)
        SDL_RenderPresent(g_r);
    }
    return 0;
}

// NativeNX's start: the settings and profiles, the command line, then a command without a window or the window's loop
int main(int argc, char **argv) {
    nxl_console_attach();   // (Windows: the output to the console it was started from, else to a log file)
    config_load(&g_cfg);
    profiles_load(&g_profs);   // who plays (the first start makes one), and their time per game
    plays_load(g_profs.p[g_profs.cur].id, &g_plays);
    args_t a;
    int r = args_parse(argc, argv, &a);
    if (r >= 0) return r;
    if (a.list) return cmd_list();
    ach_store_load(g_profs.p[g_profs.cur].id, &g_ach);
    if (a.ach) return cmd_achievements(a.ach);
    if (a.run) return cmd_run(a.run);
    if (window_start(&a)) return 1;
    first_screen(&a);
    r = main_loop(&a);
    if (g_child) game_close();   // NativeNX closing: the game it holds too
    SDL_Quit();
    return r;
}
