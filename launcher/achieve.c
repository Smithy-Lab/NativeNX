// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's achievements (achieve.h)
#include "achieve.h"
#include "paths.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// ---------- the lists ----------
static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t l = strlen(s);
    while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
    return s;
}
// Reads an achievements list (.ini): its game, save file, each achievement's name, points, condition, and named lists.
static int load_file(const char *path, ach_set_t *s, int general) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *line = malloc(1 << 16);   // a named list can be long (BotW's compendium: 394 names)
    ach_def_t *d = NULL;
    int cap = s->n;
    while (fgets(line, 1 << 16, f)) {
        char *l = trim(line);
        if (!*l || *l == '#' || *l == ';') continue;
        if (*l == '[') {
            char *e = strchr(l, ']');
            if (!e) continue;
            *e = 0;
            if (s->n == cap) s->a = realloc(s->a, (size_t)(cap = cap ? 2 * cap : 32) * sizeof *s->a);
            d = &s->a[s->n++];
            memset(d, 0, sizeof *d);
            snprintf(d->id, sizeof d->id, "%s%s", general ? "general." : "", trim(l + 1));
            d->general = general;
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(l), *v = trim(eq + 1);
        if (!d) {   // the file's own keys, before the first [achievement]
            if (general) continue;
            if (!strcmp(k, "title")) snprintf(s->title, sizeof s->title, "%s", v);
            else if (!strcmp(k, "save")) snprintf(s->save_file, sizeof s->save_file, "%s", v);
            else if (!strcmp(k, "format")) s->fmt = !strcmp(v, "flags32") ? 1 : 0;
            else if (!strncmp(k, "list.", 5) && s->nlists < ACH_MAX_LISTS) {   // a named list: count(@name)
                snprintf(s->lists[s->nlists].name, sizeof s->lists[0].name, "%s", k + 5);
                s->lists[s->nlists++].items = strdup(v);
            }
            continue;
        }
        if (!strcmp(k, "name")) snprintf(d->name, sizeof d->name, "%s", v);
        else if (!strcmp(k, "desc")) snprintf(d->desc, sizeof d->desc, "%s", v);
        else if (!strcmp(k, "points")) d->points = atoi(v);
        else if (!strcmp(k, "hidden")) d->hidden = atoi(v);
        else if (!strcmp(k, "dlc")) d->dlc = atoi(v);
        else if (!strcmp(k, "when")) snprintf(d->when, sizeof d->when, "%s", v);
    }
    free(line);
    fclose(f);
    return 0;
}
// A game's achievements: its own list (achievements/<program ID>.ini), then the general ones every game has.
int ach_load(uint64_t program_id, const char *data, ach_set_t *s) {
    memset(s, 0, sizeof *s);
    s->program_id = program_id;
    char p[1200];
    snprintf(p, sizeof p, "%s/achievements/%016llX.ini", data, (unsigned long long)program_id);
    if (program_id) load_file(p, s, 0);
    snprintf(p, sizeof p, "%s/achievements/general.ini", data);
    load_file(p, s, 1);
    for (int i = 0; i < s->n; i++) s->total_points += s->a[i].points;
    return 0;
}
void ach_free(ach_set_t *s) {
    for (int i = 0; i < s->nlists; i++) free(s->lists[i].items);
    free(s->a);
    memset(s, 0, sizeof *s);
}

// ---------- the saves ----------
uint32_t ach_crc32(const char *s) {
    static uint32_t t[256];
    if (!t[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; }
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) c = t[(c ^ (uint8_t)*s) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
typedef struct { uint32_t h, v, at; } hv_t;
static int by_hash(const void *a, const void *b) {
    const hv_t *x = a, *y = b;
    if (x->h != y->h) return x->h < y->h ? -1 : 1;
    return x->at < y->at ? -1 : x->at > y->at;   // an array's first element first
}
// Reads a save file of 32-bit flags (BotW's game_data.sav: name hash, value pairs) into a slot, sorted by hash.
static int read_flags32(const char *path, ach_slot_t *o) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 20 || sz > (64 << 20)) { fclose(f); return -1; }
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return -1; }
    fclose(f);
    size_t n = (size_t)(sz - 12) / 8;
    hv_t *e = malloc(n * sizeof *e);
    for (size_t i = 0; i < n; i++) {
        const uint8_t *q = b + 12 + 8 * i;
        e[i].h = (uint32_t)q[0] | (uint32_t)q[1] << 8 | (uint32_t)q[2] << 16 | (uint32_t)q[3] << 24;
        e[i].v = (uint32_t)q[4] | (uint32_t)q[5] << 8 | (uint32_t)q[6] << 16 | (uint32_t)q[7] << 24;
        e[i].at = (uint32_t)i;
    }
    free(b);
    qsort(e, n, sizeof *e, by_hash);
    o->h = malloc((n ? n : 1) * sizeof *o->h), o->v = malloc((n ? n : 1) * sizeof *o->v), o->n = 0;
    for (size_t i = 0; i < n; i++) {
        if (o->n && o->h[o->n - 1] == e[i].h) continue;   // later elements of an array
        o->h[o->n] = e[i].h, o->v[o->n] = e[i].v, o->n++;
    }
    free(e);
    return 0;
}
// the save file in the folder and in its sub-folders (a game's save slots), with a signature of their sizes and times
// a file's last change, to the nanosecond where the system keeps it
static uint64_t file_time(const struct stat *st) {
#ifdef _WIN32
    return (uint64_t)st->st_mtime * 1000000007u;
#else
    return (uint64_t)st->st_mtim.tv_sec * 1000000007u ^ (uint64_t)st->st_mtim.tv_nsec;
#endif
}
static int find_saves(const char *dir, const char *name, char out[][1400], int max, uint64_t *sig) {
    int n = 0;
    struct stat st;
    char p[1400];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    if (!stat(p, &st) && S_ISREG(st.st_mode)) { snprintf(out[n++], 1400, "%s", p); *sig = *sig * 1000003u ^ (uint64_t)st.st_size ^ file_time(&st); }
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "%s/%s/%s", dir, e->d_name, name);
        if (!stat(p, &st) && S_ISREG(st.st_mode)) { snprintf(out[n++], 1400, "%s", p); *sig = *sig * 1000003u ^ (uint64_t)st.st_size ^ file_time(&st); }
    }
    if (d) closedir(d);
    return n;
}
void ach_saves_free(ach_saves_t *sv) {
    for (int i = 0; i < sv->n; i++) free(sv->slot[i].h), free(sv->slot[i].v);
    free(sv->slot);
    sv->slot = NULL, sv->n = 0;
}
// Reads the game's save slots for its achievements again, when the files changed since the last read.
int ach_saves_read(const ach_set_t *s, const char *save_dir, ach_saves_t *sv) {
    if (s->fmt != 1 || !s->save_file[0]) return 0;
    static char paths[32][1400];
    uint64_t sig = 1469598103934665603ull;
    int n = find_saves(save_dir, s->save_file, paths, 32, &sig);
    if (sig == sv->sig && (n > 0) == (sv->n > 0)) return 0;
    ach_saves_free(sv);
    sv->sig = sig;
    sv->slot = calloc((size_t)(n ? n : 1), sizeof *sv->slot);
    for (int i = 0; i < n; i++) if (!read_flags32(paths[i], &sv->slot[sv->n])) sv->n++;
    return 1;
}
// A flag's value in a save slot, by its name's hash (binary search); 1 when the save has it.
static int slot_get(const ach_slot_t *sl, uint32_t h, int32_t *v) {
    int lo = 0, hi = sl->n - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (sl->h[m] == h) { *v = (int32_t)sl->v[m]; return 1; }
        if (sl->h[m] < h) lo = m + 1; else hi = m - 1;
    }
    return 0;
}

// ---------- conditions ----------
typedef struct {
    const char *p;
    const ach_ctx_t *c;
    const ach_slot_t *sl;   // the save slot looked at (NULL: none)
    int err;
    int depth;              // named lists inside named lists: not followed far
} ev_t;
typedef struct { double v; int tracked; double prog, target; } val_t;   // tracked: 1, 0 (no flag found), -2 (no save)
static void ws(ev_t *e) { while (isspace((unsigned char)*e->p)) e->p++; }
// Reads a name (letters, digits, _ and .) from the condition being evaluated.
static int word(ev_t *e, char *out, size_t n) {
    ws(e);
    size_t k = 0;
    while ((isalnum((unsigned char)*e->p) || *e->p == '_' || *e->p == '.') && k + 1 < n) {
        if (e->p[0] == '.' && e->p[1] == '.') break;
        out[k++] = *e->p++;
    }
    out[k] = 0;
    return (int)k;
}
static int flag_val(ev_t *e, const char *name, int32_t *v) {
    if (!e->sl) return -2;
    return slot_get(e->sl, ach_crc32(name), v);
}
// one name of a list, or a run A..B: the names whose number runs from A's to B's. The number is where A and B differ
// (IsPlayed_Demo126_0..IsPlayed_Demo138_0), or the digits at A's end when B is the number alone (Clear_Dungeon000..119);
// as wide as A's
static void item_eval(ev_t *e, const char *a, const char *b, int sum, val_t *r) {
    int32_t v;
    if (!b[0]) { if (flag_val(e, a, &v) == 1) { r->tracked = 1; r->v += sum ? v : (v != 0); } return; }
    size_t la = strlen(a), lb = strlen(b), p0, p1, q1;   // A = pre + A's digits [p0, p1) + post; B's digits [p0, q1)
    int num_only = 1;
    for (size_t i = 0; i < lb; i++) if (!isdigit((unsigned char)b[i])) num_only = 0;
    if (num_only) {
        p0 = la;
        while (p0 && isdigit((unsigned char)a[p0 - 1])) p0--;
        if (p0 == la) return;
        long from = atol(a + p0), to = atol(b);
        for (long i = from; i <= to && i - from < 10000; i++) {
            char nm[260];
            snprintf(nm, sizeof nm, "%.*s%0*ld", (int)p0, a, (int)(la - p0), i);
            if (flag_val(e, nm, &v) == 1) { r->tracked = 1; r->v += sum ? v : (v != 0); }
        }
        return;
    }
    size_t c = 0;
    while (c < la && c < lb && a[c] == b[c]) c++;
    p0 = c;
    while (p0 && isdigit((unsigned char)a[p0 - 1])) p0--;   // back to the start of the number
    p1 = p0, q1 = p0;
    while (p1 < la && isdigit((unsigned char)a[p1])) p1++;
    while (q1 < lb && isdigit((unsigned char)b[q1])) q1++;
    if (p1 == p0 || q1 == p0 || strcmp(a + p1, b + q1)) { e->err = 1; return; }   // not a run of one number
    long from = atol(a + p0), to = atol(b + p0);
    for (long i = from; i <= to && i - from < 10000; i++) {
        char nm[260];
        snprintf(nm, sizeof nm, "%.*s%0*ld%s", (int)p0, a, (int)(p1 - p0), i, a + p1);
        if (flag_val(e, nm, &v) == 1) { r->tracked = 1; r->v += sum ? v : (v != 0); }
    }
}
static const ach_set_t *g_set;   // the set evaluated now (its named lists)
static void list_items(ev_t *e, int sum, val_t *r, int close);
// Evaluates a named list of the game's file (count / sum over its items).
static void named_list(ev_t *e, const char *name, int sum, val_t *r) {
    for (int i = 0; g_set && i < g_set->nlists; i++) {
        if (strcmp(g_set->lists[i].name, name)) continue;
        if (e->depth > 4) { e->err = 1; return; }
        ev_t sub = *e;
        sub.p = g_set->lists[i].items, sub.depth = e->depth + 1;
        list_items(&sub, sum, r, 0);
        if (sub.err) e->err = 1;
        return;
    }
    e->err = 1;   // no such list
}
// the names of a list, to its closing bracket (close) or its end
static void list_items(ev_t *e, int sum, val_t *r, int close) {
    for (;;) {
        ws(e);
        if (*e->p == '@') {
            e->p++;
            char n[64];
            if (!word(e, n, sizeof n)) { e->err = 1; return; }
            named_list(e, n, sum, r);
        } else {
            char a[200], b[200] = "";
            if (!word(e, a, sizeof a)) { e->err = 1; return; }
            ws(e);
            if (e->p[0] == '.' && e->p[1] == '.') { e->p += 2; ws(e); word(e, b, sizeof b); }
            item_eval(e, a, b, sum, r);
        }
        if (e->err) return;
        ws(e);
        if (*e->p == '|' || *e->p == ',') { e->p++; continue; }
        if (close && *e->p == ')') { e->p++; return; }
        if (!close && !*e->p) return;
        e->err = 1;
        return;
    }
}
// Evaluates count(...) or sum(...): how many of the items are set, or their values added.
static void list_eval(ev_t *e, int sum, val_t *r) {
    r->v = 0, r->tracked = e->sl ? 0 : -2;
    ws(e);
    if (*e->p != '(') { e->err = 1; return; }
    e->p++;
    list_items(e, sum, r, 1);
    if (r->tracked == 0 && !e->sl) r->tracked = -2;
}
static val_t ev_or(ev_t *e);
// Evaluates a value in a condition: a number, a flag, count / sum / f32(...), or a parenthesised condition.
static val_t ev_value(ev_t *e) {
    val_t r = {0, 1, 0, 0};
    ws(e);
    if (*e->p == '(') { e->p++; r = ev_or(e); ws(e); if (*e->p == ')') e->p++; else e->err = 1; return r; }
    if (isdigit((unsigned char)*e->p)) {   // a number, unless it is a name that starts with digits (100enemy_Clear_Junior)
        const char *q = e->p;
        while (isdigit((unsigned char)*q) || (*q == '.' && q[1] != '.')) q++;
        if (!isalpha((unsigned char)*q) && *q != '_') { r.v = strtod(e->p, (char **)&e->p); return r; }
    }
    char w[200];
    if (!word(e, w, sizeof w)) { e->err = 1; return r; }
    if (!strcmp(w, "count") || !strcmp(w, "sum")) { list_eval(e, w[0] == 's', &r); return r; }
    if (!strcmp(w, "f32")) {   // f32(NAME): the flag's value as a float (BotW's StaminaMax)
        char n[200];
        ws(e);
        if (*e->p != '(') { e->err = 1; return r; }
        e->p++;
        if (!word(e, n, sizeof n)) { e->err = 1; return r; }
        ws(e);
        if (*e->p != ')') { e->err = 1; return r; }
        e->p++;
        int32_t v;
        int k = flag_val(e, n, &v);
        float f;
        memcpy(&f, &v, 4);
        r.tracked = k == 1 ? 1 : k == -2 ? -2 : 0, r.v = k == 1 ? f : 0;
        return r;
    }
    if (!strcmp(w, "playtime")) { r.v = e->c->play_hours; return r; }
    if (!strcmp(w, "screenshots")) { r.v = e->c->screenshots; return r; }
    if (!strcmp(w, "launched")) { r.v = e->c->launched; return r; }
    if (!strcmp(w, "favourite")) { r.v = e->c->favourite; return r; }
    int32_t v;
    int k = flag_val(e, w, &v);
    r.tracked = k == 1 ? 1 : k == -2 ? -2 : 0;
    r.v = k == 1 ? v : 0;
    return r;
}
// Evaluates a comparison (>=, <=, ==, >, <); without one, a value is met when it is not 0.
static val_t ev_cmp(ev_t *e) {
    val_t a = ev_value(e);
    ws(e);
    const char *ops[] = {">=", "<=", "==", ">", "<"};
    for (int i = 0; i < 5; i++) {
        size_t l = strlen(ops[i]);
        if (strncmp(e->p, ops[i], l)) continue;
        e->p += l;
        val_t b = ev_value(e);
        int m = i == 0 ? a.v >= b.v : i == 1 ? a.v <= b.v : i == 2 ? a.v == b.v : i == 3 ? a.v > b.v : a.v < b.v;
        val_t r = {m, a.tracked == 1 && b.tracked == 1 ? 1 : a.tracked != 1 ? a.tracked : b.tracked, 0, 0};
        if (i == 0 || i == 3) r.prog = a.v, r.target = b.v + (i == 3);   // a counter towards a number: progress
        return r;
    }
    val_t r = {a.v != 0, a.tracked, a.v != 0, 1};
    return r;
}
// Evaluates "a and b and ...".
static val_t ev_and(ev_t *e) {
    val_t a = ev_cmp(e);
    for (;;) {
        ws(e);
        if (strncmp(e->p, "and", 3) || isalnum((unsigned char)e->p[3])) return a;
        e->p += 3;
        val_t b = ev_cmp(e);
        int t = a.tracked == 1 && b.tracked == 1 ? 1 : a.tracked == -2 || b.tracked == -2 ? -2 : 0;
        a = (val_t){a.v && b.v, t, 0, 0};
    }
}
// Evaluates "a or b or ..." (the condition's top).
static val_t ev_or(ev_t *e) {
    val_t a = ev_and(e);
    for (;;) {
        ws(e);
        if (strncmp(e->p, "or", 2) || isalnum((unsigned char)e->p[2])) return a;
        e->p += 2;
        val_t b = ev_and(e);
        int t = a.tracked == 1 || b.tracked == 1 ? 1 : a.tracked == -2 || b.tracked == -2 ? -2 : 0;
        double prog = a.target > 0 && b.target > 0 ? (a.prog / a.target > b.prog / b.target ? a.prog : b.prog) : 0, tgt = a.target > 0 && b.target > 0 ? (a.prog / a.target > b.prog / b.target ? a.target : b.target) : 0;
        a = (val_t){(a.tracked == 1 && a.v) || (b.tracked == 1 && b.v), t, prog, tgt};
    }
}
// Evaluates a condition against one save slot: met or not, and the progress toward it.
static ach_state_t eval_slot(const char *when, const ach_ctx_t *c, const ach_slot_t *sl) {
    ev_t e = {when, c, sl, 0};
    val_t r = ev_or(&e);
    ws(&e);
    ach_state_t s = {0, r.prog, r.target};
    if (e.err || *e.p) { s.state = -1; return s; }   // a condition that does not read: not tracked
    s.state = r.tracked == 1 ? (r.v != 0) : r.tracked == -2 ? -2 : -1;
    return s;
}
// Evaluates a condition against every save slot: met in any of them, and the furthest progress.
ach_state_t ach_eval_one(const char *when, const ach_ctx_t *c) {
    const ach_saves_t *sv = c->saves;
    if (!sv || !sv->n) return eval_slot(when, c, NULL);
    ach_state_t best = {-1, 0, 0};
    for (int i = 0; i < sv->n; i++) {   // every save slot: met in any, the furthest progress of any
        ach_state_t s = eval_slot(when, c, &sv->slot[i]);
        if (s.state == 1) best.state = 1;
        else if (s.state == 0 && best.state != 1) best.state = 0;
        if (s.target > 0 && (best.target <= 0 || s.value / s.target > best.value / best.target)) best.value = s.value, best.target = s.target;
    }
    return best;
}
void ach_eval(const ach_set_t *s, const ach_ctx_t *c, ach_state_t *out) {
    g_set = s;
    for (int i = 0; i < s->n; i++) out[i] = ach_eval_one(s->a[i].when, c);
    g_set = NULL;
}
ach_state_t ach_eval_in(const ach_set_t *s, const char *when, const ach_ctx_t *c) {
    g_set = s;
    ach_state_t r = ach_eval_one(when, c);
    g_set = NULL;
    return r;
}

// ---------- a profile's unlocks ----------
static void store_path(int id, char *out, size_t n, int mk) {
    char sub[64], d[1000];
    snprintf(sub, sizeof sub, "profiles/%d", id);
    nx_dir(0, sub, d, sizeof d);
    if (mk) nx_mkdirs(d);
    snprintf(out, n, "%s/achievements.ini", d);
}
void ach_store_free(ach_store_t *st) { free(st->u); memset(st, 0, sizeof *st); }
// Reads a profile's unlocked achievements.
void ach_store_load(int profile_id, ach_store_t *st) {
    ach_store_free(st);
    char p[1100];
    store_path(profile_id, p, sizeof p, 0);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        unsigned long long pid;
        char id[48];
        int pts;
        long long t;
        if (sscanf(line, "%llx\t%47s\t%d\t%lld", &pid, id, &pts, &t) != 4) continue;
        ach_unlock(st, pid, id, pts, t);
    }
    fclose(f);
}
// Writes a profile's unlocked achievements (through a temporary file).
void ach_store_save(int profile_id, const ach_store_t *st) {
    char p[1100], tmp[1200];
    store_path(profile_id, p, sizeof p, 1);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) { fprintf(stderr, "[launcher] cannot write %s: %s\n", tmp, strerror(errno)); return; }
    fprintf(f, "# NativeNX achievements: program ID, achievement, points, unlocked (seconds since 1970)\n");
    for (int i = 0; i < st->n; i++) fprintf(f, "%016llX\t%s\t%d\t%lld\n", (unsigned long long)st->u[i].pid, st->u[i].id, st->u[i].points, st->u[i].t);
    fclose(f);
    rename(tmp, p);
}
long long ach_unlocked(const ach_store_t *st, uint64_t pid, const char *id) {
    for (int i = 0; i < st->n; i++) if (st->u[i].pid == pid && !strcmp(st->u[i].id, id)) return st->u[i].t;
    return 0;
}
// Records an achievement as unlocked now (once).
void ach_unlock(ach_store_t *st, uint64_t pid, const char *id, int points, long long t) {
    if (ach_unlocked(st, pid, id)) return;
    if (st->n == st->cap) st->u = realloc(st->u, (size_t)(st->cap = st->cap ? 2 * st->cap : 32) * sizeof *st->u);
    ach_unlock_t *u = &st->u[st->n++];
    u->pid = pid, u->points = points, u->t = t ? t : 1;
    snprintf(u->id, sizeof u->id, "%s", id);
}
int ach_points(const ach_store_t *st) { int p = 0; for (int i = 0; i < st->n; i++) p += st->u[i].points; return p; }
int ach_game_points(const ach_store_t *st, uint64_t pid) { int p = 0; for (int i = 0; i < st->n; i++) if (st->u[i].pid == pid) p += st->u[i].points; return p; }
int ach_game_count(const ach_store_t *st, uint64_t pid) { int c = 0; for (int i = 0; i < st->n; i++) c += st->u[i].pid == pid; return c; }
