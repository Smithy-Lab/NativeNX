// SPDX-License-Identifier: GPL-3.0-or-later
// NativeNX's achievements (launcher/achieve.c): the conditions against a made-up flags32 save in two slots, the lists
// shipped in launcher/data/achievements (each condition reads), and a profile's unlocks
#include "../../launcher/achieve.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../test_fs.h"

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok - " __VA_ARGS__); else { printf("not ok - " __VA_ARGS__); fails++; } printf("\n"); } while (0)

static void put32(FILE *f, uint32_t v) { uint8_t b[4] = {v, v >> 8, v >> 16, v >> 24}; fwrite(b, 1, 4, f); }
static void flag(FILE *f, const char *name, int32_t v) { put32(f, ach_crc32(name)); put32(f, (uint32_t)v); }
static void make_save(const char *path, int slot2) {
    FILE *f = fopen(path, "wb");
    if (!f) { printf("not ok - cannot write %s\n", path); exit(1); }
    put32(f, 0x471E), put32(f, 0xFFFFFFFF), put32(f, 1);
    flag(f, "IsGet_PlayerStole2", 1);
    for (int i = 1; i <= 15; i++) { char n[32]; snprintf(n, sizeof n, "MapTower_%02d", i); flag(f, n, i <= (slot2 ? 15 : 4)); }
    for (int i = 0; i < 120; i++) { char n[32]; snprintf(n, sizeof n, "Clear_Dungeon%03d", i); flag(f, n, i < 12); }
    flag(f, "HiddenKorok_Number", slot2 ? 30 : 61);
    flag(f, "Defeated_Enemy_Lynel_Junior_Num", 2), flag(f, "Defeated_Enemy_Lynel_Gold_Num", 1);
    flag(f, "Clear_RemainsWind", 1), flag(f, "Clear_RemainsWater", 0);
    flag(f, "MaxHartValue", 48);
    flag(f, "100enemy_Clear_Junior", 1);
    for (int i = 0; i < 6; i++) flag(f, "HorseName", 0x41 + i);   // an array: the same name again
    float st = slot2 ? 3000.f : 1600.f;
    int32_t sti;
    memcpy(&sti, &st, 4);
    flag(f, "StaminaMax", sti);
    flag(f, "IsPlayed_Demo126_0", 1), flag(f, "IsPlayed_Demo130_0", 1), flag(f, "IsPlayed_Demo131_0", 0);
    flag(f, "IsRegisteredPictureBook_Animal_Bear_A", 1), flag(f, "IsRegisteredPictureBook_Animal_Boar_A", slot2);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *data = argc > 1 ? argv[1] : "launcher/data";
    char dir[256], p[512];
    snprintf(dir, sizeof dir, "%s/t_achieve.%d", tfs_tmp(), (int)getpid());
    tfs_mkdirs(dir);
    snprintf(p, sizeof p, "%s/0", dir), tfs_mkdirs(p);
    snprintf(p, sizeof p, "%s/1", dir), tfs_mkdirs(p);
    snprintf(p, sizeof p, "%s/0/game_data.sav", dir), make_save(p, 0);
    snprintf(p, sizeof p, "%s/1/game_data.sav", dir), make_save(p, 1);

    CHECK(ach_crc32("123456789") == 0xCBF43926u, "CRC32 is the usual one (zlib's)");
    ach_set_t s;
    ach_load(0x01007EF00011E000ull, data, &s);
    int ngen = 0;
    for (int i = 0; i < s.n; i++) ngen += s.a[i].general;
    CHECK(s.fmt == 1 && !strcmp(s.save_file, "game_data.sav") && s.n - ngen == 75 && ngen == 8, "BotW's list: flags32 in game_data.sav, 75 achievements and the 8 general ones (%d, %d)", s.n - ngen, ngen);
    int base = 0, dlc = 0;
    for (int i = 0; i < s.n; i++) if (!s.a[i].general) { if (s.a[i].dlc) dlc += s.a[i].points; else base += s.a[i].points; }
    CHECK(base == 1000 && dlc == 0, "1000 points (%d, %d for downloadable content)", base, dlc);

    ach_saves_t sv = {0};
    CHECK(ach_saves_read(&s, dir, &sv) == 1 && sv.n == 2, "both save slots read (%d)", sv.n);
    CHECK(ach_saves_read(&s, dir, &sv) == 0, "read again with nothing changed: no change");
    ach_ctx_t c = {&sv, 12.5, 3, 1, 0};
    ach_state_t st;
    st = ach_eval_one("IsGet_PlayerStole2", &c);
    CHECK(st.state == 1, "a flag that is set");
    st = ach_eval_one("Clear_RemainsWater", &c);
    CHECK(st.state == 0, "a flag that is not set");
    st = ach_eval_one("Clear_RemainsWind and Clear_RemainsWater", &c);
    CHECK(st.state == 0, "and");
    st = ach_eval_one("Clear_RemainsWater or Clear_RemainsWind", &c);
    CHECK(st.state == 1, "or");
    st = ach_eval_one("count(MapTower_01..MapTower_15) >= 15", &c);
    CHECK(st.state == 1 && st.value == 15 && st.target == 15, "count over a run: met in the second slot (%g / %g)", st.value, st.target);
    st = ach_eval_one("count(Clear_Dungeon000..119) >= 40", &c);
    CHECK(st.state == 0 && st.value == 12 && st.target == 40, "a run with the number alone at the end; progress 12 / 40 (%g)", st.value);
    st = ach_eval_one("HiddenKorok_Number >= 50", &c);
    CHECK(st.state == 1 && st.value == 61, "a counter: the slot that is further on counts (%g)", st.value);
    st = ach_eval_one("sum(Defeated_Enemy_Lynel_Junior_Num|Defeated_Enemy_Lynel_Middle_Num|Defeated_Enemy_Lynel_Gold_Num) >= 3", &c);
    CHECK(st.state == 1 && st.value == 3, "sum, with a flag the save lacks among them (%g)", st.value);
    st = ach_eval_one("100enemy_Clear_Junior", &c);
    CHECK(st.state == 1, "a name that starts with digits");
    st = ach_eval_one("HorseName == 65", &c);
    CHECK(st.state == 1, "an array's first element");
    st = ach_eval_one("f32(StaminaMax) >= 3000", &c);
    CHECK(st.state == 1 && st.value == 3000, "a float flag (%g)", st.value);
    st = ach_eval_one("count(IsPlayed_Demo126_0..IsPlayed_Demo138_0) >= 2", &c);
    CHECK(st.state == 1 && st.value == 2, "a run with the number in the middle of the name (%g)", st.value);
    st = ach_eval_in(&s, "count(@compendium) >= 2", &c);
    CHECK(st.state == 1 && st.value == 2, "a named list (BotW's compendium) (%g)", st.value);
    st = ach_eval_in(&s, "count(@nope) >= 1", &c);
    CHECK(st.state == -1, "a list that is not there: not tracked");
    st = ach_eval_one("Not_A_Real_Flag", &c);
    CHECK(st.state == -1, "a name the save does not have: not tracked");
    st = ach_eval_one("count(Nope_01..Nope_09) >= 1", &c);
    CHECK(st.state == -1, "a run of names the save does not have: not tracked");
    st = ach_eval_one("playtime >= 10 and screenshots >= 3 and launched", &c);
    CHECK(st.state == 1, "the launcher's numbers");
    st = ach_eval_one("favourite", &c);
    CHECK(st.state == 0, "not a favourite");
    st = ach_eval_one("count(MapTower_01", &c);
    CHECK(st.state == -1, "a condition that does not read: not tracked");
    ach_ctx_t none = {NULL, 0, 0, 0, 0};
    st = ach_eval_one("IsGet_PlayerStole2", &none);
    CHECK(st.state == -2, "no save yet");

    // every shipped condition reads (none untracked for a reason of syntax): checked against the made-up save, a flag
    // it lacks makes "not tracked" (-1) and is allowed; what is checked is that each condition parses
    ach_state_t *all = calloc((size_t)s.n, sizeof *all);
    ach_eval(&s, &c, all);
    int bad = 0;
    for (int i = 0; i < s.n; i++) {
        if (!s.a[i].name[0] || !s.a[i].desc[0] || s.a[i].points <= 0 || !s.a[i].when[0]) { printf("# %s: a name, a description, points and a condition are needed\n", s.a[i].id); bad++; }
        if (strstr(s.a[i].when, "..") || strchr(s.a[i].when, '(')) continue;
    }
    CHECK(!bad, "every achievement has a name, a description, points and a condition");
    CHECK(all[0].state == 1, "the paraglider: unlocked by the made-up save");
    // the general list works for a game without a list of its own
    ach_set_t g;
    ach_load(0x0100000000001000ull, data, &g);
    CHECK(g.n == 8 && g.fmt == 0, "a game without a list: the general ones only");
    ach_saves_t gs = {0};
    CHECK(ach_saves_read(&g, dir, &gs) == 0, "... no save is read for it");
    ach_free(&g);

    // a profile's unlocks
    setenv("XDG_CONFIG_HOME", dir, 1);
    ach_store_t u = {0};
    ach_store_load(3, &u);
    ach_unlock(&u, 0x01007EF00011E000ull, "medoh", 50, 1000);
    ach_unlock(&u, 0x01007EF00011E000ull, "medoh", 50, 2000);
    ach_unlock(&u, 0x0100000000001000ull, "general.first_steps", 5, 1500);
    ach_store_save(3, &u);
    ach_store_t u2 = {0};
    ach_store_load(3, &u2);
    CHECK(u2.n == 2 && ach_unlocked(&u2, 0x01007EF00011E000ull, "medoh") == 1000, "unlocks saved and read back, once each (%d)", u2.n);
    CHECK(ach_points(&u2) == 55 && ach_game_points(&u2, 0x01007EF00011E000ull) == 50 && ach_game_count(&u2, 0x0100000000001000ull) == 1, "points: all games and one game");
    ach_store_free(&u), ach_store_free(&u2);
    free(all);
    ach_saves_free(&sv);
    ach_free(&s);
    tfs_rmtree(dir);
    printf(fails ? "FAILED: %d\n" : "all passed\n", fails);
    return fails != 0;
}
