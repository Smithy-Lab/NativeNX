// SPDX-License-Identifier: GPL-3.0-or-later
// The launcher's game library: game folders under the games folder, and what is known about each.
// A game folder holds ExeFS/ (main, main.npdm, subsdk*, sdk) and RomFS/, extracted from the user's own game (an update
// already applied to them), and its DLC in DLC/<index>/ (each one's RomFS). What the launcher shows comes from the
// game's own files when they are there, else from files the user adds:
//   title, publisher, version: control.nacp (the console's title data), in the game folder or in control/ or Control/
//   picture: icon_<language>.dat (the console's 256x256 JPEG icon, next to control.nacp); else cover.png / cover.jpg
//   background: background.png / background.jpg; else the picture, blurred
//   program ID: ExeFS/main.npdm
//   nxlinux.env: KEY=VALUE lines added to nxlinux's environment for this game (NXL_*, DM_*)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "bnp.h"

typedef struct {
    char dir[1024];          // the game folder
    char exefs[1100], romfs[1100];
    char title[256], publisher[128], version[32];
    char icon[1100];         // a picture file (JPEG / PNG), or "" when there is none
    char background[1100];   // a background picture file, or ""
    uint64_t program_id;     // 0 when main.npdm has none
    int title_from_nacp;
    char env[16][256]; int nenv;
    char dlc[1100]; int ndlc;   // its DLC folder (DLC/<index>/: nxlinux's DM_AOC_DIR) and how many are in it, or ""
    int gfx;                 // what it draws with (GFX_*, gfx_api): NVN, or OpenGL / Vulkan (bridged to the PC's driver)
    int fstep;               // its main is a build nxnative's frame step knows (60 fps; nxlinux's loader.c install_fstep)   // nxlinux.env, then built-in defaults for the program (ones the file sets are not overridden)
} game_t;

// what a game draws with, from its code modules' imports (gfxapi.c): Switch games use NVN; some use OpenGL (through
// EGL) or Vulkan, whose calls nxlinux takes to the PC's own driver
enum { GFX_UNKNOWN, GFX_NVN, GFX_OPENGL, GFX_VULKAN };
int gfx_api(const char *exefs);
const char *gfx_api_name(int api);   // "NVN", "OpenGL", "Vulkan", "unknown"

typedef struct gopts gopts_t;   // a game's options (below)
// Saves backed up: <games folder>/Saves backup/<game>/<date time> <version>/, copies of the profile's save folder
int save_backup(const game_t *g, const char *save_dir, const char *label, char *made, size_t mn);   // 0 done (made: its folder), 1 no save yet, -1 failed
int save_backups(const game_t *g, char names[][128], int max);   // the game's backups, newest first
int save_restore(const game_t *g, const char *save_dir, const char *name);   // the save replaced by a backup; 0 done
void save_backup_dir(const game_t *g, char *out, size_t n);
extern char g_library_root[1024];   // the games folder of the last scan

// every game folder under root (the folder itself, its sub-folders, and theirs), sorted by title; returns the count
int library_scan(const char *root, game_t **out);
// the NACP's title for its first language with one (American English first); 0 when the file is not a NACP
int nacp_read(const char *path, char *title, size_t tn, char *publisher, size_t pn, char *version, size_t vn);
// the program ID in an NPDM (its ACI0 section); 0 when none
uint64_t npdm_program_id(const char *path);

// A game's mods: the folders in <game>/mods, each with a romfs (the files it replaces or adds) at mods/<name>/romfs, or
// in Atmosphere's layout at mods/<name>/[atmosphere/]contents/<program>/romfs. nxlinux reads a game file from the first
// enabled mod that has it, then from the game (DM_ROMFS_OVERLAY). Its exefs folder's code patches (IPSwitch .pchtxt,
// IPS files named by build ID) are applied by nxlinux's loader (NXL_EXEFS_PATCHES)
typedef struct { char name[128]; char romfs[1100]; int has_exefs; char exefs[1100];   // exefs: its code patches' folder (.pchtxt / .ips), or ""
                 int bnp; bnp_t b; } mod_t;   // bnp: a BCML mod (Breath of the Wild, bnp.h): merged with the game's files at the start
int mods_list(const game_t *g, mod_t *out, int max);
// A game's options (~/.config/nxlinux/games/<folder>.ini): native (as on the console: nothing below applies), the
// anisotropic filtering at least (0 the game's own), the frame rate, the mods turned on
// the frame rate: 0 the game's own (30 for the games that have the frame step), 60 up to 60 when the PC keeps up
// res: the resolution scale in tenths (0 or 10 = the game's own, 15, 20, 25, 30, 40: nxlinux's NXL_RES_SCALE)
// fill: the picture fills the window, stretched (for widescreen mods, whose 16:9 frame holds a wider view)
struct gopts { int native; int aniso; int fps; int res; int fill; char on[32][128]; int non;
               char bnpopt[64][256]; int nbnpopt; };   // a BCML mod's options set other than their default: "<mod>|<folder>|0 or 1"
void gopts_load(const game_t *g, gopts_t *o);
void gopts_save(const game_t *g, const gopts_t *o);
int gopts_mod_on(const gopts_t *o, const char *name);
int gopts_bnp_on(const gopts_t *o, const mod_t *m, int opt);   // a BCML mod's option on (as set, else its default)
void gopts_bnp_set(gopts_t *o, const mod_t *m, int opt, int on);
void gopts_mod_set(gopts_t *o, const char *name, int on);

// the launcher's settings (~/.config/nxlinux/launcher.ini): the games folder and an optional background picture
typedef struct {
    char games[1024]; char background[1024];
    int blur;     // the background's blur: 0 off, 1 light, 2 medium, 3 strong
    int sounds;   // interface sounds on
    int volume;   // their volume, percent
    int game_fullscreen;   // games start on the whole screen (nxlinux's NXL_FULLSCREEN; F11 there switches)
    int upscale;           // the game's frame to the screen: 0 FSR 1.0, 1 smooth (bilinear), 2 sharp pixels (NXL_UPSCALE)
    int sharpness;         // FSR's sharpening: 0 off, 1 light, 2 medium (the default), 3 strong (NXL_SHARPNESS)
    int fps_counter;   // the frame rate counter in the game's window (nxlinux's NXL_FPS; F1 there turns it on and off)
    int sound_style;   // 0 soft (made by the launcher), 1 Kenney's (launcher/data), 2 the user's own (~/.local/share/nxlinux/sounds)
} config_t;
void config_load(config_t *c);
void config_save(const config_t *c);
