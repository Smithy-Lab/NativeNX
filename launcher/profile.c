// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's profiles and their play time (profile.h)
#include "profile.h"
#include "paths.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static void mkdirs_p(const char *p) { nx_mkdirs(p); }
static void base_dir(int data, char *out, size_t n) { nx_dir(data, "", out, n); }   // ~/.config/nxlinux or ~/.local/share/nxlinux
void profile_avatar_path(int id, char *out, size_t n) {
    char b[900];
    base_dir(1, b, sizeof b);
    snprintf(out, n, "%s/profiles/%d/avatar.png", b, id);
}
static int file_exists(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

// Reads the profiles (who plays: names, avatars, the current one); a first one from the login's name when there are
// none.
void profiles_load(profiles_t *ps) {
    memset(ps, 0, sizeof *ps);
    ps->next_id = 1;
    char b[900], p[1000];
    base_dir(0, b, sizeof b);
    snprintf(p, sizeof p, "%s/profiles.ini", b);
    FILE *f = fopen(p, "r");
    int cur_id = 0;
    if (f) {
        char line[512];
        profile_t *pr = NULL;
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "current = ", 10)) cur_id = atoi(line + 10);
            else if (!strncmp(line, "next = ", 7)) ps->next_id = atoi(line + 7);
            else if (!strncmp(line, "[profile ", 9) && ps->n < PROFILE_MAX) {
                pr = &ps->p[ps->n++];
                memset(pr, 0, sizeof *pr);
                pr->id = atoi(line + 9);
            } else if (pr && !strncmp(line, "name = ", 7)) snprintf(pr->name, sizeof pr->name, "%s", line + 7);
            else if (pr && !strncmp(line, "since = ", 8)) pr->since = atoll(line + 8);
        }
        fclose(f);
    }
    for (int i = 0; i < ps->n; i++) {
        if (ps->p[i].id >= ps->next_id) ps->next_id = ps->p[i].id + 1;
        char a[1100];
        profile_avatar_path(ps->p[i].id, a, sizeof a);
        ps->p[i].has_avatar = file_exists(a);
        if (ps->p[i].id == cur_id) ps->cur = i;
    }
    if (!ps->n) {   // the first start: one profile, the login's name with a capital ("alex": "Alex")
        const char *u = getenv("USER");
        char name[64];
        snprintf(name, sizeof name, "%s", u && *u ? u : "Player");
        name[0] = (char)toupper((unsigned char)name[0]);
        profile_add(ps, name);
        ps->cur = 0;
        profiles_save(ps);
    }
}
// Writes the profiles.
void profiles_save(const profiles_t *ps) {
    char b[900], p[1000];
    base_dir(0, b, sizeof b);
    mkdirs_p(b);
    snprintf(p, sizeof p, "%s/profiles.ini", b);
    FILE *f = fopen(p, "w");
    if (!f) { fprintf(stderr, "[launcher] cannot write %s: %s\n", p, strerror(errno)); return; }
    fprintf(f, "# NativeNX profiles\ncurrent = %d\nnext = %d\n", ps->n ? ps->p[ps->cur].id : 0, ps->next_id);
    for (int i = 0; i < ps->n; i++) fprintf(f, "\n[profile %d]\nname = %s\nsince = %lld\n", ps->p[i].id, ps->p[i].name, ps->p[i].since);
    fclose(f);
}
// Adds a profile; its index, or -1 when there are already PROFILE_MAX.
int profile_add(profiles_t *ps, const char *name) {
    if (ps->n >= PROFILE_MAX) return -1;
    profile_t *p = &ps->p[ps->n];
    memset(p, 0, sizeof *p);
    p->id = ps->next_id++;
    snprintf(p->name, sizeof p->name, "%s", name && *name ? name : "Player");
    p->since = (long long)time(NULL);
    return ps->n++;
}
// Removes a profile (the last one stays); 0 when it was removed.
int profile_remove(profiles_t *ps, int i) {
    if (ps->n <= 1 || i < 0 || i >= ps->n) return -1;
    memmove(&ps->p[i], &ps->p[i + 1], (size_t)(ps->n - i - 1) * sizeof ps->p[0]);
    ps->n--;
    if (ps->cur >= ps->n || ps->cur == i) ps->cur = 0;
    else if (ps->cur > i) ps->cur--;
    return 0;
}
// The folder a profile keeps a game's save in (the first profile's save/ in the game's folder, the others' beside it).
void profile_save_dir(const profiles_t *ps, int i, const game_t *g, char *out, size_t n) {
    int id = i >= 0 && i < ps->n ? ps->p[i].id : 1;
    char where[1100];
    snprintf(where, sizeof where, "%s", g->dir);
    if (id == 1) snprintf(out, n, "%s/save", where);
    else snprintf(out, n, "%s/save-profile-%d", where, id);
}

// ---------- play time ----------
static void play_path(int id, char *out, size_t n, int mk) {
    char b[900], d[1000];
    base_dir(0, b, sizeof b);
    snprintf(d, sizeof d, "%s/profiles/%d", b, id);
    if (mk) mkdirs_p(d);
    snprintf(out, n, "%s/play.ini", d);
}
static const char *game_key(const game_t *g) { const char *s = strrchr(g->dir, '/'); return s ? s + 1 : g->dir; }
void plays_free(plays_t *pl) { free(pl->e); memset(pl, 0, sizeof *pl); }
// Reads a profile's time played and last played per game.
void plays_load(int id, plays_t *pl) {
    plays_free(pl);
    char p[1100];
    play_path(id, p, sizeof p, 0);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || !line[0]) continue;
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1 = 0;
        long long secs = 0, last = 0; int fav = 0;
        if (sscanf(t1 + 1, "%lld\t%lld\t%d", &secs, &last, &fav) < 2) continue;
        if (pl->n == pl->cap) pl->e = realloc(pl->e, (size_t)(pl->cap = pl->cap ? 2 * pl->cap : 16) * sizeof *pl->e);
        play_t *e = &pl->e[pl->n++];
        snprintf(e->key, sizeof e->key, "%s", line);
        e->secs = secs, e->last = last, e->fav = fav;
    }
    fclose(f);
}
// Writes a profile's play times (through a temporary file).
void plays_save(int id, const plays_t *pl) {
    char p[1100];
    play_path(id, p, sizeof p, 1);
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "[launcher] cannot write %s: %s\n", tmp, strerror(errno)); return; }
    fprintf(f, "# NativeNX play time: game folder, seconds played, last played (seconds since 1970), favourite\n");
    for (int i = 0; i < pl->n; i++) fprintf(f, "%s\t%lld\t%lld\t%d\n", pl->e[i].key, pl->e[i].secs, pl->e[i].last, pl->e[i].fav);
    fclose(f);
    rename(tmp, p);
}
// A game's play record for the profile (made when asked and there is none).
play_t *plays_get(plays_t *pl, const game_t *g, int make) {
    const char *k = game_key(g);
    for (int i = 0; i < pl->n; i++) if (!strcmp(pl->e[i].key, k)) return &pl->e[i];
    if (!make) return NULL;
    if (pl->n == pl->cap) pl->e = realloc(pl->e, (size_t)(pl->cap = pl->cap ? 2 * pl->cap : 16) * sizeof *pl->e);
    play_t *e = &pl->e[pl->n++];
    memset(e, 0, sizeof *e);
    snprintf(e->key, sizeof e->key, "%s", k);
    return e;
}
void plays_add_time(plays_t *pl, const game_t *g, long long secs, long long now) {
    play_t *e = plays_get(pl, g, 1);
    if (secs > 0) e->secs += secs;
    e->last = now;
}
long long plays_total(const plays_t *pl) { long long t = 0; for (int i = 0; i < pl->n; i++) t += pl->e[i].secs; return t; }
int plays_count(const plays_t *pl) { int c = 0; for (int i = 0; i < pl->n; i++) c += pl->e[i].last > 0; return c; }

static const plays_t *g_ord_pl; static const game_t *g_ord_games;
// Sorts games by when they were last played, newest first.
static int by_recent(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    play_t *pi = plays_get((plays_t *)g_ord_pl, &g_ord_games[i], 0), *pj = plays_get((plays_t *)g_ord_pl, &g_ord_games[j], 0);
    long long li = pi ? pi->last : 0, lj = pj ? pj->last : 0;
    if (li != lj) return li > lj ? -1 : 1;
    int c = strcasecmp(g_ord_games[i].title, g_ord_games[j].title);
    return c ? c : (i > j) - (i < j);
}
void plays_order(const plays_t *pl, const game_t *games, int n, int *perm) {
    for (int i = 0; i < n; i++) perm[i] = i;
    g_ord_pl = pl, g_ord_games = games;
    qsort(perm, (size_t)n, sizeof *perm, by_recent);
}
