// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX, the settings and a game's options: their categories and rows, and what each row does (nativenx.h lists the parts).
#include "nativenx.h"

// ---------- settings ----------
int g_settings, g_set_row;
SDL_Mutex *g_dlg_mu;
static char g_dlg_result[1024]; static int g_dlg_kind, g_dlg_done;   // a file dialog's answer (it may come on another thread)
// A file dialog finished (on SDL's thread): its result kept for the main loop.
void dlg_cb(void *ud, const char *const *files, int filter) {
    (void)filter;
    SDL_LockMutex(g_dlg_mu);
    g_dlg_kind = (int)(intptr_t)ud, g_dlg_done = 1;
    snprintf(g_dlg_result, sizeof g_dlg_result, "%s", files && files[0] ? files[0] : "");
    SDL_UnlockMutex(g_dlg_mu);
}
// Uses a finished file dialog's result (the games folder, a background picture).
void dlg_poll(void) {
    SDL_LockMutex(g_dlg_mu);
    int done = g_dlg_done, kind = g_dlg_kind;
    char r[1024];
    snprintf(r, sizeof r, "%s", g_dlg_result);
    g_dlg_done = 0;
    SDL_UnlockMutex(g_dlg_mu);
    if (!done || !r[0]) return;
    if (kind == 1) { snprintf(g_cfg.games, sizeof g_cfg.games, "%s", r); config_save(&g_cfg); rescan(); }
    else if (kind == 3) { void avatar_choose_done(const char *path); avatar_choose_done(r); }
    else { snprintf(g_cfg.background, sizeof g_cfg.background, "%s", r); config_save(&g_cfg); if (g_custom_bg.bg) SDL_DestroyTexture(g_custom_bg.bg); memset(&g_custom_bg, 0, sizeof g_custom_bg); }
}
// g_settings: 0 the dashboard, 1 the settings, 2 the selected game's options (the same screen, other categories)
static const char *const SET_CAT[] = {"Library", "Appearance", "Sound", "Controllers", "Display", "About"};
static const char *const OPT_CAT[] = {"Mode", "Graphics", "Mods", "DLC and saves", "Info"};
int ncat(void) { return g_settings == 2 ? 5 : 6; }
static const char *cat_name(int i) { return g_settings == 2 ? OPT_CAT[i] : SET_CAT[i]; }
int g_set_cat, g_set_col, g_set_top;   // the category; 0 the list of categories, 1 its rows; the first row shown
gopts_t g_opt;               // the options of the game being looked at (g_opt_game)
int g_opt_game = -1;
static mod_t g_mods[40];
static int g_nmods;
char g_backups[64][128];   // the game's save backups (newest first), and the one Restore would use
int g_nbackups, g_backup_i;
void opt_open(int gi) {
    g_opt_game = gi;
    gopts_load(&g_games[gi], &g_opt);
    g_nmods = mods_list(&g_games[gi], g_mods, 40);
    g_nbackups = save_backups(&g_games[gi], g_backups, 64), g_backup_i = 0;
}
// The rows of a game's options: its mode, graphics, mods (and their options), version and DLC, saves, info.
static int opt_rows(set_row *r) {
    int n = 0;
    const game_t *g = &g_games[g_opt_game];
    int nat = g_opt.native;
    switch (g_set_cat) {
    case 0:
        r[n++] = (set_row){"Run as", "", R_NATIVE};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", nat ? "Native (as on the console)" : "With your changes");
        r[n] = (set_row){"", "", R_NONE};
        snprintf(r[n].label, sizeof r[0].label, "%s", nat ? "Nothing changed: Graphics and Mods are not used" : "In use");
        n++;
        if (!nat) {
            char v[64];
            int on = 0;
            for (int i = 0; i < g_nmods; i++) on += gopts_mod_on(&g_opt, g_mods[i].name) && (g_mods[i].romfs[0] || g_mods[i].exefs[0]);
            char rs[16] = "";
            if (g_opt.res) snprintf(rs, sizeof rs, ", %d.%dx", g_opt.res / 10, g_opt.res % 10);
            snprintf(v, sizeof v, "%d mod%s on, anisotropic filtering %s%s%s", on, on == 1 ? "" : "s", g_opt.aniso > 1 ? (g_opt.aniso == 2 ? "2x" : g_opt.aniso == 4 ? "4x" : g_opt.aniso == 8 ? "8x" : "16x") : "as the game",
                     g->fstep && g_opt.fps == 60 ? ", 60 fps" : "", rs);
            snprintf(r[n - 1].value, sizeof r[0].value, "%s", v);
        }
        break;
    case 1:
        r[n] = (set_row){"Anisotropic filtering", "", R_ANISO, nat};
        snprintf(r[n].value, sizeof r[0].value, "%s", g_opt.aniso > 1 ? (g_opt.aniso == 2 ? "2x" : g_opt.aniso == 4 ? "4x" : g_opt.aniso == 8 ? "8x" : "16x") : "The game's own");
        n++;
        r[n++] = (set_row){"... sharper textures at an angle (the ground, roads)", "", R_NONE};
        r[n] = (set_row){"Frame rate", "", g->fstep ? R_FPS : R_NONE, nat || !g->fstep};
        snprintf(r[n].value, sizeof r[0].value, "%s", !g->fstep ? "Not available for this game (or this version of it)" : g_opt.fps == 60 ? "Up to 60 fps" : "30 fps (the game's own)");
        n++;
        if (g->fstep) r[n++] = (set_row){"... 60 while your PC keeps up, 30 when it does not; the game's speed stays right", "", R_NONE};
        r[n] = (set_row){"Resolution", "", R_RES, nat};
        if (g_opt.res % 10) snprintf(r[n].value, sizeof r[0].value, "%d.%dx", g_opt.res / 10, g_opt.res % 10);
        else if (g_opt.res) snprintf(r[n].value, sizeof r[0].value, "%dx", g_opt.res / 10);
        else snprintf(r[n].value, sizeof r[0].value, "The game's own");
        n++;
        r[n++] = (set_row){"... more pixels: BotW's 900p at 1.5x is 1350p, 2x 1800p, 2.5x above 4K", "", R_NONE};
        r[n] = (set_row){"Picture shape", "", R_FILL, nat};
        snprintf(r[n++].value, sizeof r[0].value, "%s", g_opt.fill ? "Fill the window (stretched)" : "The game's own (16:9)");
        r[n++] = (set_row){"... fill: for widescreen mods (such as a 21:9 mod in Mods), which draw a wider view", "", R_NONE};
        break;
    case 2:
        for (int i = 0; i < g_nmods && n < MAXROWS - 3; i++) {
            r[n] = (set_row){"", "", R_MOD + i, nat};
            snprintf(r[n].label, sizeof r[0].label, "%s", g_mods[i].name);
            const int usable = g_mods[i].romfs[0] || g_mods[i].exefs[0];
            snprintf(r[n].value, sizeof r[0].value, "%s", !usable ? "No romfs or exefs folder in it" : !gopts_mod_on(&g_opt, g_mods[i].name) ? "Off"
                     : g_mods[i].romfs[0] && g_mods[i].exefs[0] ? "On (files and code patches)" : g_mods[i].exefs[0] ? "On (code patches)" : "On");
            if (!usable) r[n].action = R_NONE;
            if (g_mods[i].bnp && usable) {   // a BCML mod: its name and version, then its options while it is on
                snprintf(r[n].label, sizeof r[0].label, "%s%s%s", g_mods[i].b.name[0] ? g_mods[i].b.name : g_mods[i].name, g_mods[i].b.version[0] ? " " : "", g_mods[i].b.version);
                if (gopts_mod_on(&g_opt, g_mods[i].name)) snprintf(r[n].value, sizeof r[0].value, "On (BCML mod: merged with the game's files at the start)");
            }
            n++;
            for (int k = 0; g_mods[i].bnp && gopts_mod_on(&g_opt, g_mods[i].name) && k < g_mods[i].b.nopt && n < MAXROWS - 4; k++) {
                r[n] = (set_row){"", "", R_BNPOPT + 16 * i + k, nat};
                snprintf(r[n].label, sizeof r[0].label, "   %s", g_mods[i].b.opt[k].name);
                snprintf(r[n++].value, sizeof r[0].value, "%s", gopts_bnp_on(&g_opt, &g_mods[i], k) ? "On" : "Off");
            }
        }
        if (!g_nmods) r[n++] = (set_row){"No mods yet", "Each one in mods/<name>/romfs or /exefs in the game's folder", R_NONE};
        r[n] = (set_row){"Open the mods folder", "", R_MODS_OPEN};
        snprintf(r[n++].value, sizeof r[0].value, "%s/mods", g->dir);
        if (g_nmods > 1) r[n++] = (set_row){"Where two mods change one file", "the first in this list wins", R_NONE};
        break;
    case 3: {   // DLC, the save's backups
        r[n] = (set_row){"DLC", "", R_NONE};
        if (g->ndlc) snprintf(r[n++].value, sizeof r[0].value, "%d in the game's DLC folder (all in use)", g->ndlc);
        else snprintf(r[n++].value, sizeof r[0].value, "None: each one's RomFS in DLC/<its number>/ in the game's folder");
        r[n] = (set_row){"Open the DLC folder", "", R_CONTENT_OPEN};
        snprintf(r[n++].value, sizeof r[0].value, "%s/DLC", g->dir);
        r[n++] = (set_row){"Back up the save", "a copy in Saves backup, with the date and version", R_BACKUP};
        r[n] = (set_row){"Backup to restore", "", g_nbackups > 1 ? R_RESTORE_PICK : R_NONE};
        snprintf(r[n++].value, sizeof r[0].value, "%s", g_nbackups ? g_backups[g_backup_i] : "None yet");
        if (g_nbackups) r[n++] = (set_row){"Restore that backup", "your save now is backed up first", R_RESTORE};
        break;
    }
    default: {
        char log[1100], logs[1000];
        r[n++] = (set_row){"Title", "", R_NONE};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", g->title);
        if (g->version[0]) { r[n] = (set_row){"Version", "", R_NONE}; snprintf(r[n++].value, sizeof r[0].value, "%s", g->version); }
        r[n] = (set_row){"Program ID", "", R_NONE};
        snprintf(r[n++].value, sizeof r[0].value, "%016llx", (unsigned long long)g->program_id);
        r[n] = (set_row){"Folder", "", R_NONE};
        snprintf(r[n++].value, sizeof r[0].value, "%s", g->dir);
        nx_dir(1, "logs", logs, sizeof logs);
        snprintf(log, sizeof log, "%s/%s.log", logs, strrchr(g->dir, '/') ? strrchr(g->dir, '/') + 1 : g->dir);
        r[n] = (set_row){"Its log", "", R_NONE};
        snprintf(r[n++].value, sizeof r[0].value, "%s", log);
        for (int i = 0; i < g->nenv && n < MAXROWS; i++) { r[n] = (set_row){"nxlinux setting", "", R_NONE}; snprintf(r[n++].value, sizeof r[0].value, "%s", g->env[i]); }
        break;
    }
    }
    return n;
}
// The rows of the settings category shown (or of a game's options).
int set_rows(set_row *r) {
    if (g_settings == 2 && g_opt_game >= 0 && g_opt_game < g_ngames) return opt_rows(r);
    int n = 0;
    switch (g_set_cat) {
    case 0:
        r[n++] = (set_row){"Games folder", "", R_GAMES};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", g_cfg.games[0] ? g_cfg.games : "Not chosen");
        r[n++] = (set_row){"Look for games again", "", R_RESCAN};
        snprintf(r[n - 1].value, sizeof r[0].value, "%d game%s found", g_ngames, g_ngames == 1 ? "" : "s");
        break;
    case 1:
        r[n++] = (set_row){"Background", "", R_BG};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", g_cfg.background[0] ? (strrchr(g_cfg.background, '/') ? strrchr(g_cfg.background, '/') + 1 : g_cfg.background) : "The selected game's picture");
        if (g_cfg.background[0]) r[n++] = (set_row){"Use each game's picture again", "", R_BG_CLEAR};
        else {
            static const char *const lv[4] = {"Off", "Light", "Medium", "Strong"};
            r[n++] = (set_row){"Background blur", "", R_BLUR};
            snprintf(r[n - 1].value, sizeof r[0].value, "%s", lv[g_cfg.blur & 3]);
        }
        r[n++] = (set_row){"Full screen", "", R_FULLSCREEN};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN ? "On" : "Off");
        break;
    case 2:
        r[n++] = (set_row){"Interface sounds", "", R_SOUNDS};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", g_cfg.sounds ? "On" : "Off");
        r[n++] = (set_row){"Volume", "", R_VOLUME};
        snprintf(r[n - 1].value, sizeof r[0].value, "%d%%", g_cfg.volume);
        static const char *const st[3] = {"Soft", "Kenney", "Your own"};
        r[n++] = (set_row){"Sound style", "", R_STYLE};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", st[g_cfg.sound_style % 3]);
        if (g_cfg.sound_style == 2) {   // where to put them
            r[n] = (set_row){"Your sounds go in", "", R_NONE};
            own_sounds_dir(r[n].value, sizeof r[0].value);
            n++;
            r[n++] = (set_row){"... named", "move, bump, confirm, back, open, close, launch, toggle, error .wav", R_NONE};
        }
        if (g_cfg.sound_style == 1) r[n++] = (set_row){"Sounds by", "Kenney (kenney.nl), CC0", R_NONE};
        break;
    case 4: {   // how games are shown
        static const char *const up[3] = {"FSR 1.0 (sharp)", "Smooth (bilinear)", "Sharp pixels"}, *const sh[4] = {"Off", "Light", "Medium", "Strong"};
        r[n++] = (set_row){"Games start", "", R_GFULL};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", g_cfg.game_fullscreen ? "Full screen" : "In a window");
        r[n++] = (set_row){"Scaling to the screen", "", R_UPSCALE};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", up[g_cfg.upscale % 3]);
        r[n] = (set_row){"FSR sharpening", "", R_SHARP, g_cfg.upscale != 0};
        snprintf(r[n++].value, sizeof r[0].value, "%s", sh[g_cfg.sharpness & 3]);
        r[n++] = (set_row){"... FSR: sharper on a 1080p, 1440p or 4K screen", "", R_NONE};
        r[n++] = (set_row){"Frame rate counter in games", "", R_FPSCOUNT};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", g_cfg.fps_counter ? "On" : "Off");
        r[n++] = (set_row){"In a game", "F11 full screen, F1 frame rate counter", R_NONE};
        break;
    }
    case 3: {
        int np = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&np);
        for (int i = 0; i < np && n < 6; i++) {
            SDL_Gamepad *p = SDL_GetGamepadFromID(ids[i]);
            const char *nm = SDL_GetGamepadNameForID(ids[i]);
            r[n] = (set_row){"", "", R_NONE};
            snprintf(r[n].label, sizeof r[0].label, "%s", nm ? nm : "Controller");
            snprintf(r[n].value, sizeof r[0].value, "%s", p && p == g_pad ? "In use" : "Connected");
            n++;
        }
        SDL_free(ids);
        if (!np) r[n++] = (set_row){"No controller connected", "Keyboard and mouse", R_NONE};
        r[n] = (set_row){"Buttons", "", R_NONE};
        snprintf(r[n].value, sizeof r[0].value, "%s confirms, %s goes back", btn("ok"), btn("back"));
        n++;
        break;
    }
    default:
        r[n++] = (set_row){"NativeNX", NXL_VERSION, R_NONE};
        r[n++] = (set_row){"Add NativeNX to the app menu", "", R_INSTALL};
        snprintf(r[n - 1].value, sizeof r[0].value, "%s", desktop_installed() ? "Added (again: A)" : "Not yet");
        r[n] = (set_row){"Settings file", "", R_NONE};
        nx_dir(0, "launcher.ini", r[n++].value, sizeof r[0].value);
        r[n] = (set_row){"Game logs", "", R_NONE};
        nx_dir(1, "logs", r[n++].value, sizeof r[0].value);
        r[n] = (set_row){"Shader and pipeline cache", "", R_NONE};
        nxl_user_dir(NXL_DIR_CACHE, r[n++].value, sizeof r[0].value);
        break;
    }
    return n;
}
int g_confirm_kind, g_confirm_yes, g_confirm_n;   // the confirm window: what it asks, the button chosen, how many
static void ask(int kind) {   // a warning with Cancel / Restore (6: restore a backup)
    g_confirm_kind = kind;
    popup_open(POP_CONFIRM);
    g_confirm_n = 2, g_confirm_yes = 1;
    LOG("asks: restore a backup");
}
static int game_busy(int gi) {   // the game is running (or waits at Home): its save stays as it is
    if (!g_child || g_play_gi != gi) return 0;
    toast("Close the game first (it is running)");
    return 1;
}
int copy_save(const game_t *g) {   // the profile's save backed up, with the version in the name; 0 done or no save
    char save[1100], made[1600], lab[80];
    profile_save_dir(&g_profs, g_profs.cur, g, save, sizeof save);
    snprintf(lab, sizeof lab, "%s", g->version[0] ? g->version : "backup");   // (the version its title data gives)
    int r = save_backup(g, save, lab, made, sizeof made);
    if (r < 0) { toast("The save could not be copied (see the terminal)"); return -1; }
    char m[300];
    snprintf(m, sizeof m, r ? "No save yet: nothing to copy" : "Save copied to Saves backup");
    toast(m);
    if (!r) LOG("save backed up: %s", made);
    if (g_opt_game >= 0 && &g_games[g_opt_game] == g) g_nbackups = save_backups(g, g_backups, 64), g_backup_i = 0;
    return 0;
}
// Does a game options row's action (cycles a value, turns a mod on or off, backs up or restores a save).
static void opt_do(int action) {
    static const int an[5] = {0, 2, 4, 8, 16};
    game_t *gg = &g_games[g_opt_game];
    if (action == R_BACKUP) { if (!game_busy(g_opt_game)) copy_save(gg); return; }
    if (action == R_RESTORE_PICK) { if (g_nbackups) g_backup_i = (g_backup_i + 1) % g_nbackups; sound(SND_MOVE); return; }
    if (action == R_RESTORE) { if (g_nbackups && !game_busy(g_opt_game)) ask(6); return; }
    if (action == R_CONTENT_OPEN) {   // the game's DLC folder (made when it is not there yet)
        char d[1200];
        snprintf(d, sizeof d, "%s/DLC", gg->dir), mkdir(d, 0755);
        open_folder(d);
        return;
    }
    if (action == R_NATIVE) { g_opt.native = !g_opt.native; sound(SND_TOGGLE); }
    else if (action == R_ANISO) { int k = 0; while (k < 4 && an[k] != g_opt.aniso) k++; g_opt.aniso = an[(k + 1) % 5]; }
    else if (action == R_FILL) { g_opt.fill = !g_opt.fill; sound(SND_TOGGLE); }
    else if (action == R_RES) { static const int rs[6] = {0, 15, 20, 25, 30, 40}; int k = 0; while (k < 5 && rs[k] != g_opt.res) k++; g_opt.res = rs[(k + 1) % 6]; }
    else if (action == R_FPS && g_games[g_opt_game].fstep) { g_opt.fps = g_opt.fps == 60 ? 0 : 60; sound(SND_TOGGLE); }
    else if (action == R_MODS_OPEN) {
        char d[1200];
        snprintf(d, sizeof d, "%s/mods", g_games[g_opt_game].dir);
        mkdir(d, 0755);
        open_folder(d);
        g_nmods = mods_list(&g_games[g_opt_game], g_mods, 40);   // look again (it may have been made just now)
        return;
    } else if (action >= R_BNPOPT && (action - R_BNPOPT) / 16 < g_nmods) {
        const mod_t *m = &g_mods[(action - R_BNPOPT) / 16];
        int k = (action - R_BNPOPT) % 16;
        if (k >= m->b.nopt) return;
        gopts_bnp_set(&g_opt, m, k, !gopts_bnp_on(&g_opt, m, k));
        sound(SND_TOGGLE);
    } else if (action >= R_MOD && action - R_MOD < g_nmods) {
        const mod_t *m = &g_mods[action - R_MOD];
        gopts_mod_set(&g_opt, m->name, !gopts_mod_on(&g_opt, m->name));
        sound(SND_TOGGLE);
    } else return;
    gopts_save(&g_games[g_opt_game], &g_opt);
}
// Does a settings row's action.
void settings_do(int action) {
    if (g_settings == 2) { opt_do(action); return; }
    static const SDL_DialogFileFilter pics[] = {{"Pictures", "png;jpg;jpeg"}};
    switch (action) {
    case R_GAMES: SDL_ShowOpenFolderDialog(dlg_cb, (void *)1, g_win, g_cfg.games[0] ? g_cfg.games : NULL, false); break;
    case R_BG: SDL_ShowOpenFileDialog(dlg_cb, (void *)2, g_win, pics, 1, NULL, false); break;
    case R_BG_CLEAR: g_cfg.background[0] = 0; config_save(&g_cfg); if (g_custom_bg.bg) SDL_DestroyTexture(g_custom_bg.bg); memset(&g_custom_bg, 0, sizeof g_custom_bg); break;
    case R_RESCAN: rescan(); break;
    case R_INSTALL: if (desktop_install()) toast("Could not add NativeNX to the app menu (see the terminal)"); else sound(SND_CONFIRM); break;
    case R_FULLSCREEN: SDL_SetWindowFullscreen(g_win, !(SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN)); break;
    case R_GFULL: g_cfg.game_fullscreen = !g_cfg.game_fullscreen; config_save(&g_cfg); sound(SND_TOGGLE); break;
    case R_UPSCALE: g_cfg.upscale = (g_cfg.upscale + 1) % 3; config_save(&g_cfg); break;
    case R_SHARP: g_cfg.sharpness = (g_cfg.sharpness + 1) & 3; config_save(&g_cfg); break;
    case R_FPSCOUNT: g_cfg.fps_counter = !g_cfg.fps_counter; config_save(&g_cfg); sound(SND_TOGGLE); break;
    case R_BLUR: g_cfg.blur = (g_cfg.blur + 1) & 3; config_save(&g_cfg); pics_reset(); break;   // Off, Light, Medium, Strong in turn
    case R_SOUNDS: g_cfg.sounds = !g_cfg.sounds; config_save(&g_cfg); sound(SND_TOGGLE); break;
    case R_STYLE: g_cfg.sound_style = (g_cfg.sound_style + 1) % 3; config_save(&g_cfg); sound_load(); sound(SND_CONFIRM); break;   // Soft, Kenney, your own in turn
    case R_VOLUME: g_cfg.volume = g_cfg.volume >= 100 ? 20 : g_cfg.volume + 20 - g_cfg.volume % 20; config_save(&g_cfg); sound(SND_CONFIRM); break;
    }
}
// s fitted into w design units: cut from the start, with an ellipsis (paths keep their end)
static void fit_left(int fid, const char *s, float w, char *out, size_t n) {
    snprintf(out, n, "%s", s);
    if (text_w(fid, out) <= w) return;
    const char *p = s;
    while (*p) {
        utf8_next(&p);
        snprintf(out, n, "\xe2\x80\xa6%s", p);
        if (text_w(fid, out) <= w) return;
    }
}
// Draws the settings (or a game's options): the categories on the left, the rows on the right.
void draw_settings(void) {
    const rgba bg = {38, 40, 46, 255}, line = {255, 255, 255, 40}, accent = C_ACCENT;
    SDL_FRect all = {0, 0, g_vw, g_vh};
    color(bg);
    SDL_RenderFillRect(g_r, &all);
    // the header and the foot
    if (g_settings == 2 && g_opt_game >= 0) {   // a game's options: its picture and title
        pic_t *p = &g_pics[g_opt_game];
        pic_load(p, g_games[g_opt_game].icon, NULL);
        if (p->tex) draw_cover(p->tex, 44, SET_HEAD / 2 - 24, 48, 48, 255); else rrect(44, SET_HEAD / 2 - 24, 48, 48, 6, title_color(g_games[g_opt_game].title));
        char t[400], fit[500];
        snprintf(t, sizeof t, "Options: %s", g_games[g_opt_game].title);
        fit_left(F_TITLE, t, g_vw - 220, fit, sizeof fit);
        text(F_TITLE, 108, SET_HEAD / 2 - 24, C_TEXT, fit);
    } else {
        icon(K_GEAR, 64, SET_HEAD / 2, 17, C_TEXT);
        text(F_TITLE, 100, SET_HEAD / 2 - 24, C_TEXT, "Settings");
    }
    rrect(40, SET_HEAD, g_vw - 80, 1, 0, line);
    rrect(40, g_vh - 72, g_vw - 80, 1, 0, line);
    float hx = g_vw - 60;
    float wa = hint_w("ok", "OK"), wb = hint_w("back", "Back") + 26;
    hint_r(hx - wa - wb, g_vh - 52, "back", "Back", C_TEXT);
    hint_r(hx - wa, g_vh - 52, "ok", "OK", C_TEXT);
    draw_pad_badge(60, g_vh - 60);
    // the categories
    rrect(SET_SIDE, SET_HEAD + 20, 1, g_vh - SET_HEAD - 112, 0, line);
    for (int i = 0; i < ncat(); i++) {
        float y = SET_HEAD + 30 + i * 64;
        int on = i == g_set_cat;
        if (on && g_set_col == 0) {   // the focus: a frame
            rrect(56, y - 4, SET_SIDE - 92, 56, 6, accent);
            rrect(59, y - 1, SET_SIDE - 98, 50, 4, (rgba){52, 56, 64, 255});
        }
        if (on) rrect(64, y + 10, 4, 28, 2, accent);
        text(F_BODY, 84, y + 10, on ? accent : C_TEXT, cat_name(i));
    }
    if (g_settings == 1 && g_set_cat == 5) {   // About: the logo under the categories
        prompt("../nativenx-logo", 92, g_vh - 72 - 190, 150, 230);
    }
    // the rows of the chosen category
    set_row rows[MAXROWS];
    int n = set_rows(rows);
    float x0 = SET_SIDE + 40, x1 = g_vw - 70;
    int vis = (int)((g_vh - SET_HEAD - 30 - 80) / SET_ROW);   // rows that fit: a longer list scrolls with the focus
    if (vis < 1) vis = 1;
    if (g_set_col == 1 && g_set_row < g_set_top) g_set_top = g_set_row;
    if (g_set_col == 1 && g_set_row >= g_set_top + vis) g_set_top = g_set_row - vis + 1;
    if (g_set_top > n - vis) g_set_top = n - vis > 0 ? n - vis : 0;
    if (g_set_top < 0) g_set_top = 0;
    if (g_set_top > 0) text(F_SMALL, x1 - 30, SET_HEAD + 6, C_DIM, "\xe2\x86\x91");   // more above / below
    if (g_set_top + vis < n) text(F_SMALL, x1 - 30, g_vh - 100, C_DIM, "\xe2\x86\x93");
    for (int i = g_set_top; i < n && i < g_set_top + vis; i++) {
        float y = SET_HEAD + 30 + (i - g_set_top) * SET_ROW;
        int focus = g_set_col == 1 && i == g_set_row;
        if (focus) {
            for (int k = 3; k >= 1; k--) rrect(x0 - 10 - 2 * k, y - 4 - 2 * k, x1 - x0 + 20 + 4 * k, SET_ROW - 8 + 4 * k, 8 + 2 * k, (rgba){accent.r, accent.g, accent.b, 30});
            rrect(x0 - 10, y - 4, x1 - x0 + 20, SET_ROW - 8, 8, accent);
            rrect(x0 - 7, y - 1, x1 - x0 + 14, SET_ROW - 14, 6, (rgba){52, 56, 64, 255});
        }
        float lw = text_w(F_BODY, rows[i].label);
        rgba lc = rows[i].dim ? (rgba){255, 255, 255, 110} : rows[i].action || focus ? C_TEXT : C_DIM, vc = rows[i].dim ? (rgba){C_ACCENT.r, C_ACCENT.g, C_ACCENT.b, 110} : rows[i].action ? accent : C_DIM;
        text(F_BODY, x0 + 8, y + 18, lc, rows[i].label);
        char v[1200];
        fit_left(F_BODY, rows[i].value, x1 - x0 - lw - 60, v, sizeof v);
        text(F_BODY, x1 - 8 - text_w(F_BODY, v), y + 18, vc, v);
        if (i + 1 < n && !focus && !(g_set_col == 1 && i + 1 == g_set_row)) rrect(x0, y + SET_ROW - 6, x1 - x0, 1, 0, line);
    }
}
