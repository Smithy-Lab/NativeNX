// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's achievements. Switch games have none of their own, so each game's list is written for it
// (launcher/data/achievements/<program ID>.ini) and checked against the game's save files: when the game saves, the
// launcher reads the save and unlocks what it now shows. A list of general ones (general.ini: time played, screenshots,
// a favourite) applies to every game. Unlocks are kept per profile with their points (like gamerscore).
//
// A list's file:
//   title = The Legend of Zelda: Breath of the Wild     (for the reader)
//   save = game_data.sav                                 the save file in the save folder or its slot folders (0/, 1/ ...)
//   format = flags32                                     its layout: flags32 = a 12-byte header, then (CRC32 of the
//                                                        flag's name, value) pairs of 32-bit little-endian words
//   [an_id]
//   name = Wings of the Wild
//   desc = Get the paraglider
//   points = 15
//   hidden = 1           (optional: the name and description show only once it is unlocked)
//   dlc = 1              (optional: needs the game's downloadable content)
//   when = IsGet_PlayerStole2
// "when" is a condition:
//   FLAG                       the flag in the save is set (not 0)
//   count(A|B|C) >= N          how many of the flags are set; A..B is a run of numbered flags (MapTower_01..MapTower_15,
//                              IsPlayed_Demo126_0..IsPlayed_Demo138_0, Clear_Dungeon000..119); @NAME is a list the
//                              file names before its first achievement (list.NAME = A|B|...)
//   f32(FLAG) >= N             a flag that holds a float
//   sum(A|B) >= N              their values added (counters)
//   FLAG >= N                  a counter (also >, <=, <, ==)
//   playtime >= H              hours played (this profile); screenshots >= N; launched; favourite
//   X and Y, X or Y            both, either (and before or; brackets group)
// A condition naming flags the save does not have is "not tracked" (a flag name that is wrong, or a game version that
// lacks it): such an achievement is shown as such rather than never unlocking.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    char id[48], name[96], desc[200];
    int points, hidden, dlc;
    char when[1024];
    int general;   // from general.ini (every game)
} ach_def_t;

enum { ACH_MAX_LISTS = 16 };
typedef struct {
    uint64_t program_id;
    char title[128], save_file[64];
    struct { char name[64]; char *items; } lists[ACH_MAX_LISTS]; int nlists;   // list.NAME = A|B|... (count(@NAME))
    int fmt;            // 0 none (general ones only), 1 flags32
    ach_def_t *a; int n;
    int total_points;
} ach_set_t;

// The game's list (if there is one) and the general ones; data: the launcher's data folder. 0 on success (a set with
// only the general ones when the game has no list)
int ach_load(uint64_t program_id, const char *data, ach_set_t *s);
void ach_free(ach_set_t *s);

// What a condition is checked against: the game's saves (read by ach_saves_read) and the launcher's numbers
typedef struct { uint32_t *h, *v; int n; } ach_slot_t;   // a save file's flags, sorted by hash
typedef struct { ach_slot_t *slot; int n; uint64_t sig; } ach_saves_t;
typedef struct {
    const ach_saves_t *saves;
    double play_hours;
    int screenshots, launched, favourite;
} ach_ctx_t;
// read the set's save file from save_dir and its slot folders; 1 when something changed since the last read (sig)
int ach_saves_read(const ach_set_t *s, const char *save_dir, ach_saves_t *sv);
void ach_saves_free(ach_saves_t *sv);

typedef struct {
    int state;          // 1 met, 0 not yet, -1 not tracked (its flags are not in the save), -2 no save yet
    double value, target;   // progress (target 0: none to show)
} ach_state_t;
void ach_eval(const ach_set_t *s, const ach_ctx_t *c, ach_state_t *out);
// one condition (tests and the check tool): state as above
ach_state_t ach_eval_one(const char *when, const ach_ctx_t *c);
ach_state_t ach_eval_in(const ach_set_t *s, const char *when, const ach_ctx_t *c);   // with the set's named lists
uint32_t ach_crc32(const char *s);

// A profile's unlocks (~/.config/nxlinux/profiles/<id>/achievements.ini): program ID, achievement, points, when
typedef struct { uint64_t pid; char id[48]; int points; long long t; } ach_unlock_t;
typedef struct { ach_unlock_t *u; int n, cap; } ach_store_t;
void ach_store_load(int profile_id, ach_store_t *st);
void ach_store_save(int profile_id, const ach_store_t *st);
void ach_store_free(ach_store_t *st);
long long ach_unlocked(const ach_store_t *st, uint64_t pid, const char *id);   // when (seconds since 1970), 0 not unlocked
void ach_unlock(ach_store_t *st, uint64_t pid, const char *id, int points, long long t);
int ach_points(const ach_store_t *st);                     // all games
int ach_game_points(const ach_store_t *st, uint64_t pid);  // one game
int ach_game_count(const ach_store_t *st, uint64_t pid);
