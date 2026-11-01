// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's parts (launcher/*.c) share this header: their types, the state more than one of them uses, and the
// functions one calls in another. Each part keeps the rest to itself (static). The parts:
//   main.c         the start: options, the window, the main loop (input, the screens drawn, the game started and watched)
//   ui_draw.c      drawing: colours, text (stb_truetype), shapes and icons, pictures and the backgrounds made on a thread
//   ui_assets.c    the launcher's own files: sounds and button pictures (launcher/data), and NativeNX in the desktop's app menu
//   input.c        controllers, keyboard and mouse into actions, done on what is showing; the battery and the clock
//   launch.c       starting a game: the loading screen, BCML mods merged first (bnp.c), nxlinux started and watched
//   achievements.c achievements: checked against the saves, the pop-ups, and a game's achievements page
//   settings.c     the settings and a game's options: their categories and rows, and what each row does
//   dashboard.c    the dashboard: the top bar, the row of games, the background, the hints
//   profiles_ui.c  profiles: the row's order, avatars, the profile page, the profiles window, name entry
//   news_album.c   What's new and the Album (screenshots)
//   home_menu.c    the menu over a running game (the Home button): drawn here, shown in the game's window
#pragma once
#define _GNU_SOURCE
#include "library.h"
#include "profile.h"
#include "achieve.h"
#include "paths.h"
#include "platform.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <strings.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "stb_truetype.h"
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#include "stb_image.h"
#include "stb_image_write.h"

#ifndef NXL_VERSION
#define NXL_VERSION "development build"
#endif
#define LOG(...) do { fprintf(stderr, "[NativeNX] " __VA_ARGS__); fputc('\n', stderr); } while (0)
enum { DW = 1280, DH = 720 };   // the layout's design size; the window scales it
enum { SCR_DASH, SCR_PROFILE, SCR_ALBUM, SCR_NEWS, SCR_ACH };            // g_screen (the settings are g_settings)
enum { POP_NONE, POP_PROFILES, POP_NAME, POP_CONFIRM };          // g_popup: a small window over the screen
// the top bar: the avatar (the profile), then What's new, Album, Controllers, Settings, Exit
enum { TOP_PROFILE, TOP_NEWS, TOP_ALBUM, TOP_PADS, TOP_SETTINGS, TOP_EXIT, TOP_N };

// ---------- colours ----------
typedef struct { uint8_t r, g, b, a; } rgba;

// ---------- text (stb_truetype, baked at the window's scale) ----------
typedef struct { int first, n; } range_t;
enum { NRANGES = 5 };   // the RANGES of codepoints baked (ui_draw.c checks the count)
typedef struct { float px; SDL_Texture *tex; stbtt_packedchar *pc[NRANGES]; float ascent; } font_t;
enum { F_SMALL, F_BODY, F_TITLE, F_CLOCK, F_BIG, F_BODYB, F_N };
enum { K_RRECT, K_GEAR, K_POWER, K_FOLDER, K_PAD_XBOX, K_PAD_PS, K_PAD_NINTENDO, K_PAD_DECK, K_PAD_KEYS, K_BOLT, K_CHECK, K_NEWS, K_ALBUM, K_STAR, K_PLUS, K_CLOCK, K_JOY, K_TROPHY, K_N };
// the controller glyphs: x in -1..1, y in -0.62..0.62; the places of the sticks, the d-pad and the face buttons
typedef struct { float lsx, lsy, rsx, rsy, dpx, dpy, btx, bty; } pad_layout;
typedef struct { int k, pw, ph, prad; SDL_Texture *t; } shape_ent;

// ---------- pictures ----------
// A game's picture is small (the console's icon is a 256 x 256 JPEG), and drawn stretched over the whole window it
// showed its pixels and JPEG blocks. The background is now made on a thread of its own, at the window's size:
//   sharp (blur off, or a background picture of the game's / one chosen in the settings): a large picture averaged
//   down, a small one smoothed a little (the JPEG blocks) and enlarged with a cubic filter (Catmull-Rom)
//   blurred: averaged down to 240 / 128 / 64 pixels across, box-blurred 1 / 2 / 3 times, then enlarged with the cubic
//   filter to 640 across, so the last stretch to the window is a smooth image stretched, without streaks
// The selected game's background, and its neighbours', are asked for ahead; until one is ready the last one stays.
typedef struct { SDL_Texture *tex, *bg; int tried, asked; } pic_t;
// the background thread: jobs in, pictures out (the main thread makes the textures)
typedef struct { int gi; unsigned gen; char file[1100]; int sharp, blur, tw; uint8_t *px; int w, h; int done; } bg_job;
// sounds: one stream per sound on the default output; playing one starts it over
enum { SND_MOVE, SND_BUMP, SND_CONFIRM, SND_BACK, SND_OPEN, SND_CLOSE, SND_LAUNCH, SND_TOGGLE, SND_ERROR, SND_ACH, SND_N };
// The soft sounds, made here: sine tones with a gentle start and a quick fade (2nd harmonic quiet, a mild low-pass),
// like the console's soft taps rather than bright clicks. Each note: start (ms), pitch from / to (Hz), length (ms),
// fade time (ms), level
typedef struct { float at, f0, f1, len, tau, amp; } note_t;
// button pictures: decoded once, then made at the size drawn (an area average: crisp at any scale)
typedef struct { char name[64]; uint8_t *px; int w, h; int pw; SDL_Texture *t; } prompt_t;
enum { HINT_ICON = 34 };
// ---------- BCML mods (Breath of the Wild): merged with the game's files before it starts (bnp.c) ----------
// The game's own files come from its RomFS folder, the DLC's from the game's DLC/<index>/ folders (the first that has a
// file gives it); the merge is kept in ~/.cache/nxlinux/bnp/<program ID>/<what it was made from>/ and made again only
// when that changes
typedef struct { const char *romfs; } stock_t;
typedef struct { char dir[32][1300]; int n; } stock_aoc_t;

// ---------- achievements ----------
// Each game's list and the general ones (achieve.c), checked against the current profile's saves of the game: when a
// game starts, every 3 seconds while it runs (when it saves, the save changes), when it ends, and when its page opens.
// A new one plays a chime and shows a pop-up at the top right: in the game's window while it runs (a picture made here,
// sent down a pipe: gfx/vk_overlay.c draws it), else in the launcher's.
typedef struct { char dir[1024]; int prof; ach_set_t set; ach_saves_t sv; ach_state_t *st; } achg_t;
typedef struct { int ai, di; } achq_t;   // a pop-up to show here: the game's entry, the achievement
// The settings are a screen of their own, as on the console: the categories on the left, the chosen one's rows on the
// right (what a row is set to in the accent colour); up / down move, right / A go into a category, left / B back out
enum { R_NONE, R_GAMES, R_RESCAN, R_BG, R_BG_CLEAR, R_FULLSCREEN, R_FPSCOUNT, R_GFULL, R_UPSCALE, R_SHARP, R_BLUR, R_SOUNDS, R_VOLUME, R_STYLE,
       R_NATIVE, R_ANISO, R_FPS, R_RES, R_FILL, R_MODS_OPEN, R_INSTALL, R_BACKUP, R_RESTORE_PICK, R_RESTORE, R_CONTENT_OPEN,
       R_MOD = 100, R_BNPOPT = 300 };   // R_BNPOPT + 16 x mod + k: a BCML mod's option k
typedef struct { char label[96]; char value[1100]; int action; int dim; } set_row;   // dim: shown, but has no effect now (native)
enum { MAXROWS = 48 };
enum { SET_HEAD = 88, SET_SIDE = 380, SET_ROW = 76 };

// ---------- input ----------
enum { A_LEFT, A_RIGHT, A_UP, A_DOWN, A_OK, A_BACK, A_MENU, A_VIEW, A_FAV, A_PROF, A_FILTER, A_ACH };   // A_ACH: L, the selected game's achievements   // A_MENU: + / Menu / Options; A_VIEW: - / View / Create; A_FAV: Y; A_PROF: X; A_FILTER: R
// what is at (x, y), in design units: the kinds below, and which one
enum { H_NONE, H_TOP, H_SELECTED, H_TILE, H_EMPTY, H_CAT, H_ROW, H_FOOT, H_OPTS, H_POP, H_OUT, H_PPBTN, H_PPGAME, H_SHOT };

// ---------- the Album ----------
// Screenshots taken with F12 in a game (nxlinux writes them to ~/.local/share/nxlinux/album/<game>/), newest first:
// small pictures made on a thread of their own, A shows one large, X deletes it, Y opens its folder
typedef struct { char path[1200], game[200]; long long mtime; uint8_t *px; int w, h, state; SDL_Texture *t; } shot_t;   // state: 0 to make, 1 making, 2 made, 3 failed
enum { AV_Y0 = 236, AV_ROW = 76 };

// ---------- the menu over a running game (the Home button) ----------
// In the game, the Home button (Guide on Xbox, PS on PlayStation) or Shift+Tab asks for it ("open W H" down the game's
// pipe). NativeNX draws it here at the window's shape, into a picture with its alpha (the game shows through the dim),
// and sends it ("menu <path>"); the game holds its own input meanwhile and passes the buttons here ("key ..."). On the
// left the game, its time this session and its achievements; the choices: Resume, Achievements (the list on the
// right, scrolled with up and down), Take a screenshot, NativeNX's Home (the game waits there: stopped, its window
// minimised; A on it resumes), Close the game (A twice).
enum { OV_RESUME, OV_ACH, OV_SHOT, OV_HOME, OV_CLOSE, OV_N };

// ---------- main.c: the start: options, the window, the main loop (input, the screens drawn, the game started and watched) ----------
extern SDL_Window *g_win;
extern SDL_Renderer *g_r;
extern float g_s;   // design units -> pixels
extern float g_vw, g_vh;   // the window in design units
extern config_t g_cfg;
extern game_t *g_games;
extern int g_ngames;
extern int g_sel;   // the selected game
extern int g_nshow;   // the games the row shows: all, or the favourites first (g_fav_only)
extern int g_fav_only;   // the row shows the favourites only
extern int g_screen, g_popup, g_pop_i, g_news_top;
extern profiles_t g_profs;   // who plays (profile.c), and the current one's time per game
extern plays_t g_plays;
extern ach_store_t g_ach;   // the current profile's achievements (achieve.c)
extern const char *const TOP_LABEL[TOP_N];
float top_x(int i);
int is_fav(int gi);
int win_px_w(void);   // ---------- main ----------
void layout(void);
int screenshot(const char *path);

// ---------- ui_draw.c: drawing: colours, text (stb_truetype), shapes and icons, pictures and the backgrounds made on a thread ----------
extern const rgba C_TEXT, C_DIM, C_ACCENT;
extern uint8_t *g_ttf, *g_ttf_bold;
extern stbtt_fontinfo g_fi, g_fi_bold;
extern const float FONT_PX[F_N];
extern pic_t *g_pics;   // one per game: its tile picture and its background
extern pic_t g_custom_bg;
extern int g_nbgq;
extern unsigned g_bg_gen;   // bumped when the games are scanned again or the blur changes: older results are dropped
extern SDL_Mutex *g_bg_mu;
void color(rgba c);
SDL_FColor fcol(rgba c);
uint8_t *find_font(int bold);
void fonts_for_scale(void);
uint32_t utf8_next(const char **s);
float text_w(int fid, const char *s);   // width of s in design units
float text(int fid, float x, float y, rgba c, const char *s);   // s with its top at y (design units); returns the width
pad_layout pad_lay(int k);
float shape_d(int k, float x, float y, float w_units, float h_units, float rad);   // distance at (x, y) in shape units (the shape's height is 2 units for icons, 1.24 for controllers)
void shape(int k, float x, float y, float w, float h, float rad, rgba c);
void rrect(float x, float y, float w, float h, float rad, rgba c);
void circle(float cx, float cy, float rad, rgba c);
void icon(int k, float cx, float cy, float r, rgba c);
void icon_folder(float cx, float cy, float r, rgba c);
SDL_Texture *tex_from_rgba(const uint8_t *px, int w, int h, int linear);
uint8_t *img_down(const uint8_t *px, int w, int h, int dw, int dh);   // an area average down to dw x dh (alpha-weighted, so no dark fringes); RGBA bytes in and out
void bg_ask(pic_t *p, int gi, const char *file, const char *bg_file);   // ask for game gi's background (gi -1: the one chosen in the settings)
void bg_collect(void);   // the finished ones become textures
void bg_init(void);
void pic_load(pic_t *p, const char *file, const char *bg_file);
void draw_cover(SDL_Texture *t, float x, float y, float w, float h, uint8_t alpha);   // fill the rectangle with the texture, cropped to keep its shape
rgba title_color(const char *t);   // a game without a picture: a colour from its title, and its initials
void draw_tile(int gi, float x, float y, float size, int selected, float glow);

// ---------- ui_assets.c: the launcher's own files: sounds and button pictures (launcher/data), and NativeNX in the desktop's app menu ----------
extern char g_data[1024];
extern uint64_t g_splash_t0;   // the logo at start: shown, then fading into the dashboard (any button skips it)
extern int g_splash_on, g_splash_hold;   // hold: --splash (pictures), the logo held at full
void data_find(void);
int export_sounds(const char *dir);   // --export-sounds DIR: the soft sounds as 16-bit WAV files (to hear them, or to start a set of one's own from)
void own_sounds_dir(char *out, size_t n);
void sound_load(void);   // the sounds of the chosen style (g_cfg.sound_style); any that style lacks are the soft ones
void sound_init(void);
void sound(int i);
void prompts_clear(void);
SDL_Texture *prompt_tex(const char *name, float size);
int prompt(const char *name, float x, float y, float size, uint8_t alpha);
int desktop_installed(void);
int desktop_install(void);
void window_icon(void);
void draw_splash(void);

// ---------- input.c: controllers, keyboard and mouse into actions, done on what is showing; the battery and the clock ----------
extern SDL_Gamepad *g_pad;   // the one used last
extern int g_confirm_east;   // its A button is the right-hand one (Nintendo's layout)
extern const char *g_fake_pad;   // --pad (pictures of the screens): the controller shown
extern int g_quit;
void pad_use(SDL_Gamepad *p);
const char *btn(const char *action);   // the face-button label for an action, in the controller's own words
float hint(float x, float y, const char *button, const char *what, rgba c);
float hint_w(const char *role, const char *what);
float hint_r(float x, float y, const char *role, const char *what, rgba c);   // the button for an action (its picture, or a drawn badge) and what it does; returns the width with a gap after
void hint_left(float *x, const char *role, const char *what, rgba c);   // hints from the right: each one drawn left of the last, x moved along
void draw_pad_badge(float x, float y);
void draw_status(float right, float y);   // ---------- the battery and the clock ----------
void select_game(int i);
void hit(float x, float y, int *kind, int *idx);
void moved(int ok);   // a move: the tick when the focus went somewhere, the bump at an edge (as the console's menus sound)
void settings_open(int on);   // 0 back to the dashboard, 1 the settings, 2 the selected game's options

int hit_more(float x, float y, int *kind, int *idx);
void act(int a);
void hold_tick(void);
void on_event(const SDL_Event *e);

// ---------- launch.c: starting a game: the loading screen, BCML mods merged first (bnp.c), nxlinux started and watched ----------
extern nxl_proc g_child;   // ---------- launching ----------
extern int g_last_status;   // how the last game ended: its exit code, or minus the signal
extern char g_log[1200], g_toast[600];
extern uint64_t g_toast_until;
extern int g_loading, g_load_gi, g_load_stage;
extern int g_play_gi;
extern uint64_t g_play_t0;   // the game played (from a launch in the window) and when its first frame came
extern uint64_t g_play_acc;   // ms played before the last pause (the game waiting at NativeNX's Home)
extern int g_suspended;   // the game waits at NativeNX's Home: stopped (SIGSTOP), its window minimised
extern uint64_t g_load_t0;
extern char g_load_last[200];
extern int g_ach_fd;   // the pipe to the running game's window: its achievement pop-ups and the menu (NXL_NOTIFY_FD)
extern int g_game_fd;   // ... and from it: the Home button, the buttons while the menu is up (NXL_LAUNCHER_FD)
extern char g_pending_launch[1024];   // a game (its folder) to start once the one closing has ended (it waited at Home)
void toast(const char *m);
void open_folder(const char *dir);
void mkdirs(const char *p);
int game_waiting(int gi);
void play_dirs_keep(char pd[1024], char ld[1024]);
void play_dirs_follow(const char *pd, const char *ld);
uint64_t play_ms(void);
void loading_poll(void);
void draw_loading(void);
void launch(const game_t *g);
void child_poll(void);

// ---------- achievements.c: achievements: checked against the saves, the pop-ups, and a game's achievements page ----------
extern int g_ach_seq, g_still;   // g_still: a picture of the window (--screenshot): a pop-up drawn where it rests
uint64_t ach_pid(const game_t *g);
achg_t *ach_game(int gi);
void ach_ctx_for(int gi, achg_t *a, ach_ctx_t *c);
int ach_check(int gi, int notify);
void ach_tick(void);
void draw_ach_popups(void);   // the pop-ups shown here, one after another: in from the right, 4.5 s, out
void ach_page_order(achg_t *a, uint64_t pid);
void ach_page_open(int gi);
void draw_ach_rows(achg_t *a, uint64_t pid, float x0, float y0, float w0, int nshow, int sel_row, int *top);
void draw_ach_page(void);
void ach_page_act(int a);

// ---------- settings.c: the settings and a game's options: their categories and rows, and what each row does ----------
extern int g_settings, g_set_row;   // ---------- settings ----------
extern SDL_Mutex *g_dlg_mu;
extern int g_set_cat, g_set_col, g_set_top;   // the category; 0 the list of categories, 1 its rows; the first row shown
extern gopts_t g_opt;   // the options of the game being looked at (g_opt_game)
extern int g_opt_game;
extern char g_backups[64][128];   // the game's save backups (newest first), and the one Restore would use
extern int g_nbackups, g_backup_i;
extern int g_confirm_kind, g_confirm_yes, g_confirm_n;   // the confirm window: what it asks, the button chosen, how many
void dlg_cb(void *ud, const char *const *files, int filter);
void dlg_poll(void);
int ncat(void);
void opt_open(int gi);
int set_rows(set_row *r);
int copy_save(const game_t *g);
void settings_do(int action);
void draw_settings(void);

// ---------- dashboard.c: the dashboard: the top bar, the row of games, the background, the hints ----------
extern int g_hover, g_mouse_top;   // the game the pointer is over (not the selected one); the top bar focus came from the pointer
extern int g_focus_top;   // the selected game; -1, or 0 Settings / 1 Power in the top bar
extern float g_scroll;   // the row's scroll, eased towards g_sel
extern int g_prev_bg;
extern float g_bg_fade;
extern int g_bg_built_w;   // the window width the backgrounds were made for
void pics_reset(void);
void rescan(void);
void draw_background(void);
void draw_toast(void);   // a message over any screen, a few seconds (the game ended badly, nxlinux missing, a profile change held)
float dash_dy(void);
void draw_dashboard(void);

// ---------- profiles_ui.c: profiles: the row's order, avatars, the profile page, the profiles window, name entry ----------
extern int g_pp_row, g_pp_btn, g_pp_game;   // the profile page: the row (0 its buttons, 1 recently played), the button, the game
extern char g_name_buf[24];
extern int g_name_mode, g_name_key, g_name_shift;   // name entry: 0 a new profile, 1 rename
extern const char *const PP_BTN[4];
void order_games(int keep);
void fav_toggle(void);
void fav_filter(void);
void avatar_set(int i, const char *src);   // a chosen picture becomes the profile's avatar: the middle square, 256 x 256, as PNG
void popup_open(int kind);
void popup_close(void);
void screen_open(int s);
int recent_games(int *out, int max);   // the recently played games of the current profile, newest first: indices into g_games
void hours_text(long long secs, char *out, size_t n);
void draw_topbar(void);   // the top bar: the avatar (the profiles), then What's new, Album, Controllers, Settings, Exit
void top_do(int i);
void draw_profile_page(void);
void profile_page_act(int a);
void name_done(void);
void name_type(const char *t);
void popup_act(int a);
void draw_popup(void);

// ---------- news_album.c: What's new and the Album (screenshots) ----------
extern int g_nshots, g_al_i, g_al_full;
void draw_news(void);
void album_scan(void);
void avatar_choose_done(const char *path);
void album_delete(void);
void draw_album(void);
void album_act(int a);

// ---------- home_menu.c: the menu over a running game (the Home button): drawn here, shown in the game's window ----------
extern int g_ov_open, g_ov_sel, g_ov_list, g_ov_row, g_ov_top, g_ov_w, g_ov_h, g_ov_close_armed, g_ov_seq, g_ov_nin;
void ov_game_ended(void);
void game_close(void);
void game_resume(void);   // from NativeNX's Home back into the game
void game_poll(void);
