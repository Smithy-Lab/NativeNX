// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's profiles: who plays. Each has a name, a picture (avatar), the day it was made, and per game the time
// played, when it was last played and whether it is a favourite. Each profile has its own saves: the first one keeps the
// game folder's save folder (the saves made before profiles existed), the others save-profile-<id> beside it.
//   ~/.config/nxlinux/profiles.ini                 the profiles, and which one is in use
//   ~/.config/nxlinux/profiles/<id>/play.ini        its time per game
//   ~/.local/share/nxlinux/profiles/<id>/avatar.png its picture (a square copy of the one chosen)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "library.h"

enum { PROFILE_MAX = 16 };
typedef struct {
    int id;              // 1, 2, ... (never reused)
    char name[64];
    long long since;     // made (seconds since 1970)
    int has_avatar;      // avatar.png is there
} profile_t;
typedef struct {
    profile_t p[PROFILE_MAX];
    int n, cur;          // cur: the index in use
    int next_id;
} profiles_t;

// the profiles; the first start makes one, named after the user's login (as typed with a capital)
void profiles_load(profiles_t *ps);
void profiles_save(const profiles_t *ps);
int profile_add(profiles_t *ps, const char *name);   // its index, -1 when full
int profile_remove(profiles_t *ps, int i);           // not the last one; its saves stay on disk. 0 done
void profile_avatar_path(int id, char *out, size_t n);
// the save folder of a game for a profile: <game>/save for the first profile, <game>/save-profile-<id> for the others
void profile_save_dir(const profiles_t *ps, int i, const game_t *g, char *out, size_t n);

// a profile's time per game, keyed by the game's folder name
typedef struct { char key[256]; long long secs, last; int fav; } play_t;
typedef struct { play_t *e; int n, cap; } plays_t;
void plays_load(int id, plays_t *pl);
void plays_save(int id, const plays_t *pl);
void plays_free(plays_t *pl);
play_t *plays_get(plays_t *pl, const game_t *g, int make);   // NULL when not there (make 0)
void plays_add_time(plays_t *pl, const game_t *g, long long secs, long long now);
long long plays_total(const plays_t *pl);
int plays_count(const plays_t *pl);   // games played at all
// the dashboard's order: last played first, then the games never played by title; perm[i] = the game shown i-th
void plays_order(const plays_t *pl, const game_t *games, int n, int *perm);
