// SPDX-License-Identifier: GPL-3.0-or-later
// A game's files and its DLC from extracted folders (DM_ROMFS, DM_AOC_DIR/<index>/): the RomFS is the game's with an
// update applied, and the DLC is owned and mounts, with mods' DLC files over it when they are (DM_AOC_OVERLAY: a file
// replaced, one added).
#include "nx.h"
Result fs_mount_rom(const char *name, void *cache, u64 size) __asm__("_ZN2nn2fs8MountRomEPKcPvm");
Result fs_open(u64 *h, const char *path, int mode) __asm__("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
Result fs_read(u64 *n, u64 h, s64 off, void *buf, u64 size) __asm__("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
void fs_close(u64 h) __asm__("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
int aoc_count(void) __asm__("_ZN2nn3aoc17CountAddOnContentEv");
int aoc_list(int *out, int off, int n) __asm__("_ZN2nn3aoc16ListAddOnContentEPiii");
Result fs_query_aoc(u64 *out, int index) __asm__("_ZN2nn2fs31QueryMountAddOnContentCacheSizeEPmi");
Result fs_mount_aoc(const char *name, int index, void *cache, u64 size) __asm__("_ZN2nn2fs17MountAddOnContentEPKciPvm");
static int same(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }
static int read_all(const char *path, char *buf, int n) {
    u64 h = 0, got = 0;
    if (fs_open(&h, path, 1)) return -1;
    int r = fs_read(&got, h, 0, buf, (u64)n) ? -1 : (int)got;
    fs_close(h);
    return r;
}
int test_main(void) {
    char b[64];
    CHECK("MountRom", fs_mount_rom("rom", NULL, 0) == 0);
    CHECK("a file the update changed: its new bytes", read_all("rom:/data/hello.txt", b, sizeof b) == 21 && same(b, "HELLO from the patch\n", 21));
    CHECK("a file the update added", read_all("rom:/zzz/new.txt", b, sizeof b) == 13 && same(b, "new in 2.0.0\n", 13));
    CHECK("a file it left: the game's", read_all("rom:/data/other.txt", b, sizeof b) == 2 && same(b, "x\n", 2));
    int list[8] = {0};
    CHECK("the DLC is owned: one, index 2", aoc_count() == 1 && aoc_list(list, 0, 8) == 1 && list[0] == 2);
    u64 cache = 0;
    CHECK("its cache size", fs_query_aoc(&cache, 2) == 0 && cache > 0);
    CHECK("MountAddOnContent", fs_mount_aoc("aoc", 2, NULL, cache) == 0);
    CHECK("a DLC file", read_all("aoc:/dlc/kept.txt", b, sizeof b) == 5 && same(b, "kept\n", 5));
    if (read_all("aoc:/dlc/extra.txt", b, sizeof b) >= 0) {   // (run with mods' DLC files over it)
        CHECK("a DLC file a mod replaces: the mod's", read_all("aoc:/dlc/item.txt", b, sizeof b) == 9 && same(b, "mod item\n", 9));
        CHECK("a file a mod adds to the DLC", read_all("aoc:/dlc/extra.txt", b, sizeof b) == 10 && same(b, "mod extra\n", 10));
    } else CHECK("a DLC file (no mods over it)", read_all("aoc:/dlc/item.txt", b, sizeof b) == 8 && same(b, "dlc two\n", 8));
    CHECK("a DLC not owned does not mount", fs_mount_aoc("aoc3", 3, NULL, cache) != 0);
    return 0;
}
