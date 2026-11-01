// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, controllers, keyboard and mouse into actions, done on what is showing; the battery and the clock
// (nativenx.h lists the parts).
#include "nativenx.h"

// ---------- controllers ----------
SDL_Gamepad *g_pad;          // the one used last
int g_confirm_east;          // its A button is the right-hand one (Nintendo's layout)
// Makes a controller the one in use (its button pictures shown: Xbox, PlayStation, Nintendo, Steam Deck).
void pad_use(SDL_Gamepad *p) {
    if (!p || p == g_pad) return;
    g_pad = p;
    SDL_GamepadButtonLabel s = SDL_GetGamepadButtonLabel(p, SDL_GAMEPAD_BUTTON_SOUTH);
    g_confirm_east = !(s == SDL_GAMEPAD_BUTTON_LABEL_A || s == SDL_GAMEPAD_BUTTON_LABEL_CROSS);
    LOG("controller: %s (%s layout)", SDL_GetGamepadName(p) ? SDL_GetGamepadName(p) : "?", g_confirm_east ? "Nintendo" : "Xbox / PlayStation");
}
const char *g_fake_pad;   // --pad (pictures of the screens): the controller shown
// The kind of controller in use, for its picture.
static const char *pad_kind(void) {
    if (g_fake_pad) return g_fake_pad;
    if (!g_pad) return "Keyboard";
    const char *n = SDL_GetGamepadName(g_pad);
    if (n && strstr(n, "Steam Deck")) return "Steam Deck";
    switch (SDL_GetGamepadType(g_pad)) {
    case SDL_GAMEPAD_TYPE_XBOX360: case SDL_GAMEPAD_TYPE_XBOXONE: return "Xbox";
    case SDL_GAMEPAD_TYPE_PS3: case SDL_GAMEPAD_TYPE_PS4: case SDL_GAMEPAD_TYPE_PS5: return "PlayStation";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO: return "Pro Controller";
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT: case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return "Joy-Con";
    default: return "Controller";
    }
}
// the face-button label for an action, in the controller's own words
const char *btn(const char *action) {
    int ps = !strcmp(pad_kind(), "PlayStation");
    if (!strcmp(pad_kind(), "Keyboard")) return !strcmp(action, "ok") ? "Enter" : !strcmp(action, "back") ? "Esc" : !strcmp(action, "menu") ? "O" : !strcmp(action, "view") ? "S"
                                              : !strcmp(action, "fav") ? "F" : !strcmp(action, "prof") ? "P" : !strcmp(action, "filter") ? "G" : !strcmp(action, "ach") ? "T" : "?";
    {   // the top and left face buttons, the right shoulder: by their place on the controller (SDL's north / west)
        int nin = !strcmp(pad_kind(), "Pro Controller") || !strcmp(pad_kind(), "Joy-Con");
        if (!strcmp(action, "fav")) return ps ? "Triangle" : nin ? "X" : "Y";
        if (!strcmp(action, "prof")) return ps ? "Square" : nin ? "Y" : "X";
        if (!strcmp(action, "filter")) return ps ? "R1" : nin ? "R" : "RB";
        if (!strcmp(action, "ach")) return ps ? "L1" : nin ? "L" : "LB";
    }
    if (!strcmp(action, "ok")) return ps ? "\xc3\x97" : "A";   // ×
    if (!strcmp(action, "back")) return ps ? "O" : "B";
    if (!strcmp(action, "menu")) return ps ? "Options" : !strcmp(pad_kind(), "Pro Controller") || !strcmp(pad_kind(), "Joy-Con") ? "+" : "Menu";
    if (!strcmp(action, "view")) return ps ? "Create" : !strcmp(pad_kind(), "Pro Controller") || !strcmp(pad_kind(), "Joy-Con") ? "-" : "View";
    return "?";
}
static int pad_family(void) {   // which glyph: K_PAD_*
    const char *k = pad_kind();
    return !strcmp(k, "Keyboard") ? K_PAD_KEYS : !strcmp(k, "PlayStation") ? K_PAD_PS : !strcmp(k, "Steam Deck") ? K_PAD_DECK
         : !strcmp(k, "Pro Controller") || !strcmp(k, "Joy-Con") ? K_PAD_NINTENDO : K_PAD_XBOX;
}
// a face button's colour on this controller: Xbox's letters (A green, B red, X blue, Y yellow), PlayStation's symbols
static rgba button_color(const char *b) {
    int f = pad_family();
    if (f == K_PAD_XBOX) return !strcmp(b, "A") ? (rgba){96, 200, 70, 255} : !strcmp(b, "B") ? (rgba){235, 70, 60, 255} : !strcmp(b, "X") ? (rgba){60, 140, 240, 255} : !strcmp(b, "Y") ? (rgba){250, 200, 40, 255} : C_TEXT;
    if (f == K_PAD_PS) return !strcmp(b, "\xc3\x97") ? (rgba){120, 160, 240, 255} : !strcmp(b, "O") ? (rgba){240, 90, 90, 255} : C_TEXT;
    return (rgba){20, 24, 34, 255};
}
float hint(float x, float y, const char *button, const char *what, rgba c) {   // a round button badge and its action
    float bw = fmaxf(30, text_w(F_SMALL, button) + 16);
    int dark = pad_family() == K_PAD_XBOX || pad_family() == K_PAD_PS || pad_family() == K_PAD_DECK;   // their buttons are dark, with coloured marks
    if (dark) { rrect(x, y, bw, 30, 15, (rgba){255, 255, 255, (uint8_t)(c.a * 0.85f)}); rrect(x + 2, y + 2, bw - 4, 26, 13, (rgba){28, 32, 40, 255}); }
    else rrect(x, y, bw, 30, 15, (rgba){255, 255, 255, (uint8_t)(c.a * 0.9f)});
    rgba bc = dark ? button_color(button) : (rgba){20, 24, 34, 255};
    text(F_SMALL, x + (bw - text_w(F_SMALL, button)) / 2, y + 4, bc, button);
    return bw + 10 + text(F_BODY, x + bw + 10, y + 1, c, what) + 26;
}
static void draw_pad_glyph(int f, float x, float y, float w, rgba c, int marks) {   // the controller, w wide
    float h = w * 0.62f;
    shape(f, x, y, w, h, 0, c);
    if (!marks || f == K_PAD_KEYS || f == K_PAD_DECK) return;
    pad_layout L = pad_lay(f);
    float u = h / 1.24f, cx = x + w / 2, cy = y + h / 2;
    static const rgba xb[4] = {{96, 200, 70, 255}, {235, 70, 60, 255}, {250, 200, 40, 255}, {60, 140, 240, 255}};   // A B Y X
    static const rgba ps[4] = {{120, 160, 240, 255}, {240, 90, 90, 255}, {80, 210, 180, 255}, {230, 130, 200, 255}};
    for (int i = 0; i < 4; i++) {
        float bx = L.btx + (i == 1 ? 0.12f : i == 3 ? -0.12f : 0), by = L.bty + (i == 0 ? 0.12f : i == 2 ? -0.12f : 0);
        rgba bc = f == K_PAD_XBOX ? xb[i] : f == K_PAD_PS ? ps[i] : (rgba){200, 200, 210, 255};
        circle(cx + bx * u, cy + by * u, 0.055f * u, bc);
    }
    if (f == K_PAD_XBOX) circle(cx, cy - 0.25f * u, 0.075f * u, (rgba){96, 200, 70, 255});
}
// Kenney's pictures of this controller's buttons and of the controller (launcher/data/prompts); NULL: draw our own
static const char *prompt_name(const char *role) {
    const char *k = pad_kind();
    int ok = !strcmp(role, "ok"), back = !strcmp(role, "back"), menu = !strcmp(role, "menu"), ctrl = !strcmp(role, "ctrl");
    int view = !strcmp(role, "view");
    if (!strcmp(k, "Keyboard")) return ok ? "keyboard_enter" : back ? "keyboard_escape" : menu ? "keyboard_o" : view ? "keyboard_s" : NULL;
    if (view) return !strcmp(k, "PlayStation") ? "playstation5_button_create" : !strcmp(k, "Pro Controller") || !strcmp(k, "Joy-Con") ? "switch_button_minus"
                   : !strcmp(k, "Steam Deck") ? "steamdeck_button_view" : "xbox_button_view";
    if (!strcmp(k, "PlayStation")) return ok ? "playstation_button_color_cross" : back ? "playstation_button_color_circle" : menu ? "playstation5_button_options" : ctrl ? "controller_playstation5" : NULL;
    if (!strcmp(k, "Pro Controller") || !strcmp(k, "Joy-Con"))
        return ok ? "switch_button_a" : back ? "switch_button_b" : menu ? "switch_button_plus" : ctrl ? (!strcmp(k, "Joy-Con") ? "controller_switch" : "controller_switch_pro") : NULL;
    if (!strcmp(k, "Steam Deck")) return ok ? "steamdeck_button_a" : back ? "steamdeck_button_b" : menu ? "steamdeck_button_options" : ctrl ? "controller_steamdeck" : NULL;
    return ok ? "xbox_button_color_a" : back ? "xbox_button_color_b" : menu ? "xbox_button_menu" : ctrl ? "controller_xboxseries" : NULL;
}
float hint_w(const char *role, const char *what) {   // the width hint_r takes
    if (prompt_tex(prompt_name(role), HINT_ICON)) return HINT_ICON + 8 + text_w(F_BODY, what);
    const char *b = btn(role);
    return fmaxf(30, text_w(F_SMALL, b) + 16) + 10 + text_w(F_BODY, what);
}
// the button for an action (its picture, or a drawn badge) and what it does; returns the width with a gap after
float hint_r(float x, float y, const char *role, const char *what, rgba c) {
    if (prompt(prompt_name(role), x, y - 2, HINT_ICON, c.a)) return HINT_ICON + 8 + text(F_BODY, x + HINT_ICON + 8, y + 1, c, what) + 26;
    return hint(x, y, btn(role), what, c);
}
// hints from the right: each one drawn left of the last, x moved along
void hint_left(float *x, const char *role, const char *what, rgba c) { *x -= hint_w(role, what); hint_r(*x, g_vh - 70, role, what, c); *x -= 26; }
void draw_pad_badge(float x, float y) {
    if (!prompt(prompt_name("ctrl"), x - 4, y - 18, 72, 235)) draw_pad_glyph(pad_family(), x, y - 2, 72, (rgba){255, 255, 255, 235}, 1);
    text(F_SMALL, x + 86, y + 10, C_DIM, pad_kind());
}

// ---------- the battery and the clock ----------
void draw_status(float right, float y) {
    int secs, pct;
    SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);
    float x = right;
    if (st == SDL_POWERSTATE_ON_BATTERY || st == SDL_POWERSTATE_CHARGING || st == SDL_POWERSTATE_CHARGED) {   // only with a battery
        x -= 46;
        rgba c = pct >= 0 && pct <= 15 && st == SDL_POWERSTATE_ON_BATTERY ? (rgba){255, 80, 80, 255} : (rgba){90, 230, 120, 255};
        rrect(x, y + 6, 40, 20, 4, (rgba){255, 255, 255, 230});
        rrect(x + 2, y + 8, 36, 16, 3, (rgba){20, 24, 34, 255});
        rrect(x + 3, y + 9, 34 * (pct < 0 ? 1 : pct / 100.f), 14, 2, c);
        rrect(x + 40, y + 11, 4, 10, 2, (rgba){255, 255, 255, 230});
        if (st == SDL_POWERSTATE_CHARGING) icon(K_BOLT, x + 20, y + 16, 9, (rgba){255, 230, 80, 255});
        if (pct >= 0) {
            char b[16];
            snprintf(b, sizeof b, "%d%%", pct);
            x -= text_w(F_BODY, b) + 10;
            text(F_BODY, x, y + 2, C_TEXT, b);
        }
        x -= 26;
    }
    if (g_pad) {   // the controller's battery, when it reports one (wireless pads)
        int pp = -1;
        SDL_PowerState ps = SDL_GetGamepadPowerInfo(g_pad, &pp);
        if ((ps == SDL_POWERSTATE_ON_BATTERY || ps == SDL_POWERSTATE_CHARGING) && pp >= 0) {
            char b[16];
            snprintf(b, sizeof b, "%d%%", pp);
            x -= text_w(F_BODY, b);
            text(F_BODY, x, y + 2, pp <= 15 ? (rgba){255, 90, 90, 255} : C_TEXT, b);
            x -= 40;
            icon(K_JOY, x + 16, y + 17, 15, C_TEXT);
            x -= 26;
        }
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char clk[16];
    strftime(clk, sizeof clk, "%H:%M", &tm);
    x -= text_w(F_CLOCK, clk);
    text(F_CLOCK, x, y, C_TEXT, clk);
    {   // the profile's achievement points, beside the clock
        char pts[24];
        int p = ach_points(&g_ach);
        if (p >= 1000) snprintf(pts, sizeof pts, "%d,%03d", p / 1000, p % 1000); else snprintf(pts, sizeof pts, "%d", p);
        x -= 30 + text_w(F_BODYB, pts);
        text(F_BODYB, x, y + 3, C_TEXT, pts);
        icon(K_TROPHY, x - 18, y + 17, 13, (rgba){255, 200, 60, 255});
    }
}
int g_quit;
void select_game(int i) {
    if (i < 0 || i >= g_nshow || i == g_sel) return;
    g_prev_bg = g_sel, g_bg_fade = 0, g_sel = i;
}
// What is under the mouse at (x, y) on the screen showing: its kind and which one.
void hit(float x, float y, int *kind, int *idx) {
    *kind = H_NONE, *idx = -1;
    if (g_settings) {
        int i = (int)floorf((y - SET_HEAD - 26) / 64);
        if (x < SET_SIDE && y > SET_HEAD && i >= 0 && i < ncat()) { *kind = H_CAT, *idx = i; return; }
        set_row rows[MAXROWS];
        int n = set_rows(rows);
        i = (int)floorf((y - SET_HEAD - 26) / SET_ROW) + g_set_top;
        if (x > SET_SIDE && y > SET_HEAD && y < g_vh - 80 && i >= 0 && i < n) { *kind = H_ROW, *idx = i; return; }
        if (y > g_vh - 72 && x > g_vw / 2) *kind = H_FOOT;
        return;
    }
    if (g_popup) { hit_more(x, y, kind, idx); return; }
    if (y > 28 && y < 98) {   // the top bar: the nearest icon (top_x), so moving along it never falls between two
        int best = -1;
        float bd = 1e9f;
        for (int i = 0; i < TOP_N; i++) { float d = fabsf(x - top_x(i)); if (d < bd) bd = d, best = i; }
        if (x > top_x(TOP_PROFILE) - 40 && x < top_x(TOP_N - 1) + 32) { *kind = H_TOP, *idx = best; return; }   // across the bar's width: no gaps
        (void)bd;
    }
    if (g_screen) { hit_more(x, y, kind, idx); return; }
    if (y > g_vh - 80 && x > g_vw - 320) { *kind = H_FOOT; return; }
    if (!g_ngames) { if (y > 250 + dash_dy() && y < 420 + dash_dy() && x > 80 && x < 900) *kind = H_EMPTY; return; }
    if (x > 78 && x < 338 && y > 176 + dash_dy() && y < 436 + dash_dy()) { *kind = H_SELECTED, *idx = g_sel; return; }
    float ox = 356 + hint_w("ok", "Start") + 26;   // the Options hint under the title
    if (y > 222 + dash_dy() && y < 264 + dash_dy() && x > ox && x < ox + hint_w("menu", "Options")) { *kind = H_OPTS; return; }
    const float small = 160, gap = 14, row_y = 182 + dash_dy() + 248 - small;
    if (y < row_y || y > row_y + small) return;
    for (int i = 0; i < g_ngames; i++) {
        if (i == g_sel) continue;
        float pos = i - g_scroll, tx = pos > 0 ? 356 + (pos - 1) * (small + gap) : 84 - 22 + pos * (small + gap);
        if (x >= tx && x < tx + small) { *kind = H_TILE, *idx = i; return; }
    }
}
// a move: the tick when the focus went somewhere, the bump at an edge (as the console's menus sound)
void moved(int ok) { sound(ok ? SND_MOVE : SND_BUMP); }
// 0 back to the dashboard, 1 the settings, 2 the selected game's options
void settings_open(int on) {
    if (on == g_settings) return;
    if (on == 2) { if (!g_ngames) return; opt_open(g_sel); }
    g_settings = on, g_set_col = 0, g_set_cat = 0, g_set_row = 0, g_set_top = 0;
    sound(on ? SND_OPEN : SND_CLOSE);
}

// ---------- the mouse over pages and windows, held buttons repeating, the stick as a direction ----------
// what the pointer is over in a window (the profiles, name entry, a question) or on a page (the profile, the album):
// the same places draw_popup / draw_profile_page / draw_album draw them
int hit_more(float x, float y, int *kind, int *idx) {
    *kind = H_NONE, *idx = -1;
    if (g_popup) {
        float w = g_popup == POP_PROFILES ? 560 : g_popup == POP_NAME ? 700 : g_confirm_n == 3 ? 700 : 560;
        float h = g_popup == POP_PROFILES ? 110 + 64.f * (g_profs.n + 1) + 50 : g_popup == POP_NAME ? 470 : 250;
        float px = (g_vw - w) / 2, py = (g_vh - h) / 2;
        if (x < px || x > px + w || y < py || y > py + h) { *kind = H_OUT; return 1; }
        if (g_popup == POP_PROFILES) {
            for (int i = 0; i <= g_profs.n; i++) { float ry = py + 90 + 64.f * i; if (y >= ry - 6 && y < ry + 52 && x > px + 20 && x < px + 540) { *kind = H_POP, *idx = i; return 1; } }
        } else if (g_popup == POP_NAME) {
            for (int r = 0; r < 5; r++) for (int c = 0; c < (r < 4 ? 10 : 4); c++) {
                float kw = r < 4 ? 56 : 148, kx = px + 32 + c * (r < 4 ? 64 : 160), ky = py + 156 + r * 56;
                if (x >= kx && x < kx + kw && y >= ky && y < ky + 48) { *kind = H_POP, *idx = r * 10 + c; return 1; }
            }
        } else if (g_popup == POP_CONFIRM) {
            float pw = g_confirm_n == 3 ? 700 : 560, bw = g_confirm_n == 3 ? 190 : 130, gap = g_confirm_n == 3 ? 20 : 30;
            for (int i = 0; i < g_confirm_n; i++) {
                float bx = px + (pw - (g_confirm_n * bw + (g_confirm_n - 1) * gap)) / 2 + i * (bw + gap);
                if (x >= bx && x < bx + bw && y >= py + 160 && y < py + 208) { *kind = H_POP, *idx = i; return 1; }
            }
        }
        return 1;
    }
    if (g_screen == SCR_PROFILE) {
        float bx = 345, by = 312;
        for (int i = 0; i < 4; i++) {
            float bw = text_w(F_SMALL, PP_BTN[i]) + 32;
            if (x >= bx && x < bx + bw && y >= by && y < by + 36) { *kind = H_PPBTN, *idx = i; return 1; }
            bx += bw + 14;
        }
        int rg[12], nr = recent_games(rg, 12), first = g_pp_row == 1 && g_pp_game > 4 ? g_pp_game - 4 : 0;
        for (int k = first; k < nr; k++) {
            float tx = 84 + (k - first) * (178 + 28);
            if (x >= tx && x < tx + 178 && y >= 460 && y < 460 + 178) { *kind = H_PPGAME, *idx = k; return 1; }
        }
    } else if (g_screen == SCR_ALBUM && g_nshots && !g_al_full) {
        const float tw = 260, th = 146, gap = 22, x0 = (g_vw - 4 * tw - 3 * gap) / 2;
        int top = g_al_i / 4 > 1 ? g_al_i / 4 - 1 : 0;
        for (int i = top * 4; i < g_nshots && i < top * 4 + 12; i++) {
            float sx = x0 + (i % 4) * (tw + gap), sy = 140 + (i / 4 - top) * (th + gap);
            if (x >= sx && x < sx + tw && y >= sy && y < sy + th) { *kind = H_SHOT, *idx = i; return 1; }
        }
    }
    return 1;
}
// the pointer over a place in a window or on a page: the focus goes there, as the controller's would (1: it moved)
static int focus_more(int k, int i) {
    int ch = 0;
    if (k == H_POP) {
        if (g_popup == POP_PROFILES) ch = g_pop_i != i, g_pop_i = i;
        else if (g_popup == POP_NAME) ch = g_name_key != i, g_name_key = i;
        else if (g_popup == POP_CONFIRM) ch = g_confirm_yes != i, g_confirm_yes = i;
    } else if (k == H_PPBTN) ch = g_pp_row != 0 || g_pp_btn != i || g_focus_top >= 0, g_pp_row = 0, g_pp_btn = i, g_focus_top = -1;
    else if (k == H_PPGAME) ch = g_pp_row != 1 || g_pp_game != i || g_focus_top >= 0, g_pp_row = 1, g_pp_game = i, g_focus_top = -1;
    else if (k == H_SHOT) ch = g_al_i != i || g_focus_top >= 0, g_al_i = i, g_focus_top = -1;
    if (ch) sound(SND_MOVE);
    return ch;
}
// held directions repeat: after 350 ms, then every 110 ms
static int g_held = -1; static uint64_t g_held_next;
static void hold(int a, int down) {
    if (down) { act(a); g_held = a, g_held_next = SDL_GetTicks() + 350; }
    else if (g_held == a) g_held = -1;
}
void hold_tick(void) {
    if (g_held >= 0 && g_held <= A_DOWN && SDL_GetTicks() >= g_held_next) { act(g_held); g_held_next = SDL_GetTicks() + 110; }
}
static int g_axis_dir[2];   // the left stick as a direction: -1 / 0 / 1 per axis


// ---------- actions: a button, key or click turned into one, done on what is showing ----------
// Does an action (a button, key or click turned into one) on whatever is showing: the loading screen, a menu, a page,
// the dashboard.
void act(int a) {
    if (g_loading) {   // the loading screen: Back stops the game
        if (a == A_BACK && g_child) { nxl_proc_signal(g_child, NXL_PROC_TERM); sound(SND_BACK); }
        return;
    }
    if (g_popup) { popup_act(a); return; }
    if (g_settings) {
        set_row rows[MAXROWS];
        int n = set_rows(rows);
        if (g_set_col == 0) {
            if (a == A_UP) { moved(g_set_cat > 0); if (g_set_cat > 0) g_set_cat--, g_set_row = 0; }
            else if (a == A_DOWN) { moved(g_set_cat < ncat() - 1); if (g_set_cat < ncat() - 1) g_set_cat++, g_set_row = 0; }
            else if ((a == A_RIGHT || a == A_OK) && n) { g_set_col = 1, g_set_row = 0; sound(a == A_OK ? SND_CONFIRM : SND_MOVE); }
            else if (a == A_LEFT) moved(0);
            else if (a == A_BACK || a == A_MENU || a == A_VIEW) settings_open(0);
        } else {
            if (a == A_UP) { moved(g_set_row > 0); if (g_set_row > 0) g_set_row--; }
            else if (a == A_DOWN) { moved(g_set_row < n - 1); if (g_set_row < n - 1) g_set_row++; }
            else if (a == A_OK && g_set_row < n) { sound(rows[g_set_row].action == R_NONE ? SND_BUMP : SND_CONFIRM); settings_do(rows[g_set_row].action); }
            else if (a == A_LEFT || a == A_BACK) { g_set_col = 0; sound(a == A_BACK ? SND_BACK : SND_MOVE); }
            else if (a == A_RIGHT) moved(0);
            else if (a == A_MENU || a == A_VIEW) settings_open(0);
        }
        return;
    }
    if (a == A_PROF && g_screen != SCR_ALBUM) { popup_open(POP_PROFILES); return; }   // X: the profiles, from anywhere
    if (g_focus_top >= 0) {   // the top bar
        if (a == A_LEFT) { moved(g_focus_top > 0); if (g_focus_top > 0) g_focus_top--; }
        else if (a == A_RIGHT) { moved(g_focus_top < TOP_N - 1); if (g_focus_top < TOP_N - 1) g_focus_top++; }
        else if (a == A_DOWN || a == A_BACK) { g_focus_top = -1; if (g_screen == SCR_PROFILE) g_pp_row = 0; sound(a == A_BACK ? SND_BACK : SND_MOVE); }
        else if (a == A_UP) moved(0);
        else if (a == A_OK) { int t = g_focus_top; g_focus_top = -1; top_do(t); }
        return;
    }
    if (g_screen == SCR_PROFILE) { profile_page_act(a); return; }
    if (g_screen == SCR_ALBUM) { album_act(a); return; }
    if (g_screen == SCR_ACH) { ach_page_act(a); return; }
    if (g_screen == SCR_NEWS) {
        if (a == A_BACK) screen_open(SCR_DASH);
        else if (a == A_DOWN) { g_news_top++; moved(1); }
        else if (a == A_UP) { if (g_news_top > 0) g_news_top--, moved(1); else { g_focus_top = TOP_NEWS; moved(1); } }
        return;
    }
    if (a == A_MENU) { settings_open(g_ngames ? 2 : 1); return; }   // + on a game: its options (as on the console)
    if (a == A_VIEW) { settings_open(1); return; }
    if (a == A_FILTER) { if (g_ngames) fav_filter(); return; }
    if (a == A_ACH) { if (g_nshow) { sound(SND_OPEN); ach_page_open(g_sel); } else moved(0); return; }
    if (a == A_FAV) { if (g_nshow) fav_toggle(); else moved(0); return; }
    if (!g_ngames || !g_nshow) {
        if (a == A_OK && !g_ngames) { sound(SND_CONFIRM); settings_do(R_GAMES); }
        else if (a == A_UP) { g_focus_top = TOP_SETTINGS; moved(1); }
        else if (a != A_BACK) moved(0);
        return;
    }
    if (a == A_LEFT) { moved(g_sel > 0); select_game(g_sel - 1); }
    else if (a == A_RIGHT) { moved(g_sel < g_nshow - 1); select_game(g_sel + 1); }
    else if (a == A_UP) { g_focus_top = TOP_PROFILE; moved(1); }
    else if (a == A_DOWN) moved(0);
    else if (a == A_OK) {
        if (g_child && g_suspended && g_sel == g_play_gi) { sound(SND_LAUNCH); game_resume(); return; }   // the game waiting here: back into it
        if (g_child) { g_confirm_kind = 3; popup_open(POP_CONFIRM); return; }   // another: the one waiting closes first
        sound(SND_LAUNCH); SDL_Delay(120); launch(&g_games[g_sel]);
    }
}
// Handles an SDL event: quit, the window, keys, controller buttons and sticks, the mouse.
void on_event(const SDL_Event *e) {
    if (g_splash_on && (e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN)) { g_splash_on = 0; return; }   // a press skips the logo
    switch (e->type) {
    case SDL_EVENT_QUIT: g_quit = 1; break;
    case SDL_EVENT_TEXT_INPUT: if (g_popup == POP_NAME) name_type(e->text.text); break;
    case SDL_EVENT_KEY_DOWN:
        if (e->key.repeat && !(g_popup == POP_NAME && e->key.key == SDLK_BACKSPACE)) break;
        g_pad = NULL;
        if (g_popup == POP_NAME) {   // typing a name: the keyboard's own keys, the arrows still move on the on-screen one
            if (e->key.key == SDLK_BACKSPACE) { size_t l = strlen(g_name_buf); if (l) g_name_buf[l - 1] = 0; break; }
            if (e->key.key == SDLK_RETURN || e->key.key == SDLK_KP_ENTER) { name_done(); break; }
            if (e->key.key == SDLK_ESCAPE) { popup_close(); break; }
            if (e->key.key != SDLK_LEFT && e->key.key != SDLK_RIGHT && e->key.key != SDLK_UP && e->key.key != SDLK_DOWN) break;
        }
        switch (e->key.key) {
        case SDLK_LEFT: hold(A_LEFT, 1); break;
        case SDLK_RIGHT: hold(A_RIGHT, 1); break;
        case SDLK_UP: hold(A_UP, 1); break;
        case SDLK_DOWN: hold(A_DOWN, 1); break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: act(A_OK); break;
        case SDLK_ESCAPE: case SDLK_BACKSPACE: act(A_BACK); break;
        case SDLK_S: act(A_VIEW); break;
        case SDLK_O: act(A_MENU); break;
        case SDLK_F: act(A_FAV); break;
        case SDLK_P: act(A_PROF); break;
        case SDLK_G: act(A_FILTER); break;
        case SDLK_T: act(A_ACH); break;
        case SDLK_F11: SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)); break;
        }
        break;
    case SDL_EVENT_KEY_UP:
        if (e->key.key == SDLK_LEFT) hold(A_LEFT, 0); else if (e->key.key == SDLK_RIGHT) hold(A_RIGHT, 0);
        else if (e->key.key == SDLK_UP) hold(A_UP, 0); else if (e->key.key == SDLK_DOWN) hold(A_DOWN, 0);
        break;
    case SDL_EVENT_GAMEPAD_ADDED: { SDL_Gamepad *p = SDL_OpenGamepad(e->gdevice.which); if (p && !g_pad) pad_use(p); break; }
    case SDL_EVENT_GAMEPAD_REMOVED: if (g_pad && SDL_GetGamepadID(g_pad) == e->gdevice.which) g_pad = NULL; break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        int down = e->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        pad_use(SDL_GetGamepadFromID(e->gbutton.which));
        int b = e->gbutton.button;
        int ok = g_confirm_east ? SDL_GAMEPAD_BUTTON_EAST : SDL_GAMEPAD_BUTTON_SOUTH, back = g_confirm_east ? SDL_GAMEPAD_BUTTON_SOUTH : SDL_GAMEPAD_BUTTON_EAST;
        if (b == SDL_GAMEPAD_BUTTON_DPAD_LEFT) hold(A_LEFT, down);
        else if (b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT) hold(A_RIGHT, down);
        else if (b == SDL_GAMEPAD_BUTTON_DPAD_UP) hold(A_UP, down);
        else if (b == SDL_GAMEPAD_BUTTON_DPAD_DOWN) hold(A_DOWN, down);
        else if (down && b == ok) act(A_OK);
        else if (down && b == back) act(A_BACK);
        else if (down && b == SDL_GAMEPAD_BUTTON_START) act(A_MENU);
        else if (down && b == SDL_GAMEPAD_BUTTON_BACK) act(A_VIEW);
        else if (down && b == SDL_GAMEPAD_BUTTON_NORTH) act(A_FAV);
        else if (down && b == SDL_GAMEPAD_BUTTON_WEST) act(A_PROF);
        else if (down && b == SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER) act(A_FILTER);
        else if (down && b == SDL_GAMEPAD_BUTTON_LEFT_SHOULDER) act(A_ACH);
        else if (down && b == SDL_GAMEPAD_BUTTON_GUIDE && g_suspended) { sound(SND_LAUNCH); game_resume(); }   // Home: back into the waiting game
        break;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (e->gaxis.axis != SDL_GAMEPAD_AXIS_LEFTX && e->gaxis.axis != SDL_GAMEPAD_AXIS_LEFTY) break;
        pad_use(SDL_GetGamepadFromID(e->gaxis.which));
        int ax = e->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY, v = e->gaxis.value, d = v > 20000 ? 1 : v < -20000 ? -1 : (abs(v) < 12000 ? 0 : g_axis_dir[ax]);
        if (d == g_axis_dir[ax]) break;
        if (g_axis_dir[ax]) hold(ax ? (g_axis_dir[ax] < 0 ? A_UP : A_DOWN) : (g_axis_dir[ax] < 0 ? A_LEFT : A_RIGHT), 0);
        g_axis_dir[ax] = d;
        if (d) hold(ax ? (d < 0 ? A_UP : A_DOWN) : (d < 0 ? A_LEFT : A_RIGHT), 1);
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (g_screen == SCR_NEWS && !g_popup) { if (e->wheel.y < 0) g_news_top++; else if (g_news_top > 0) g_news_top--; break; }   // the wheel scrolls
        if (g_screen == SCR_ALBUM && !g_popup && !g_al_full && g_nshots) { int d = e->wheel.y < 0 ? 4 : -4; if (g_al_i + d >= 0 && g_al_i + d < g_nshots) g_al_i += d, sound(SND_MOVE); break; }
        if (g_screen || g_popup) break;
        if (!g_settings && !g_popup && !g_screen) { int d = e->wheel.y < 0 || e->wheel.x > 0 ? 1 : -1; moved(g_sel + d >= 0 && g_sel + d < g_ngames); select_game(g_sel + d); } break;
    case SDL_EVENT_MOUSE_MOTION: {   // what the pointer is over is highlighted, as the controller's focus would be
        int k, i;
        hit(e->motion.x / g_s, e->motion.y / g_s, &k, &i);
        if (g_settings) {
            if (k == H_CAT && (g_set_cat != i || g_set_col != 0)) { g_set_cat = i, g_set_col = 0, g_set_row = 0; sound(SND_MOVE); }
            else if (k == H_ROW && (g_set_col != 1 || g_set_row != i)) { g_set_col = 1, g_set_row = i; sound(SND_MOVE); }
            break;
        }
        if (g_popup) { focus_more(k, i); break; }
        if (g_screen && k != H_TOP) {
            if (g_mouse_top) { g_focus_top = -1, g_mouse_top = 0; if (g_screen == SCR_PROFILE && g_pp_row < 0) g_pp_row = 0; }
            focus_more(k, i);
            break;
        }
        if (k == H_TOP && g_focus_top != i) { g_focus_top = i; g_mouse_top = 1; if (g_screen == SCR_PROFILE) g_pp_row = -1; sound(SND_MOVE); }
        else if (k != H_TOP && g_mouse_top) { g_focus_top = -1; g_mouse_top = 0; }
        int hv = k == H_TILE ? i : -1;
        if (hv != g_hover && hv >= 0) sound(SND_MOVE);
        g_hover = hv;
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        if (e->button.button == SDL_BUTTON_RIGHT && (g_popup || g_screen || g_settings)) { act(A_BACK); break; }   // the right button: back
        if (e->button.button != SDL_BUTTON_LEFT) break;
        int k, i;
        hit(e->button.x / g_s, e->button.y / g_s, &k, &i);
        if (g_settings) {
            set_row rows[MAXROWS];
            int n = set_rows(rows);
            if (k == H_CAT) { g_set_cat = i, g_set_col = 0, g_set_row = 0; sound(SND_CONFIRM); }
            else if (k == H_ROW && i < n) { g_set_col = 1, g_set_row = i; sound(rows[i].action == R_NONE ? SND_BUMP : SND_CONFIRM); settings_do(rows[i].action); }
            else if (k == H_FOOT) settings_open(0);
            break;
        }
        if (k == H_OUT) { popup_close(); break; }   // a click outside a window closes it
        if (k == H_POP || k == H_PPBTN || k == H_PPGAME || k == H_SHOT) { focus_more(k, i); act(A_OK); break; }
        if (g_popup) break;
        if (g_screen == SCR_ALBUM && g_al_full && k != H_TOP) { act(A_OK); break; }   // the picture shown large: a click closes it
        if (k == H_TOP) { g_focus_top = -1; top_do(i); }
        else if (g_screen) break;
        else if (k == H_EMPTY) { sound(SND_CONFIRM); settings_do(R_GAMES); }
        else if (k == H_SELECTED) { sound(SND_LAUNCH); SDL_Delay(120); launch(&g_games[g_sel]); }   // the selected game: a click starts it (a tap, on the console)
        else if (k == H_TILE) { select_game(i); g_hover = -1; sound(SND_MOVE); }
        else if (k == H_FOOT) settings_open(1);
        else if (k == H_OPTS) settings_open(2);
        break;
    }
    }
}
