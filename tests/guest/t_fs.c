// SPDX-License-Identifier: GPL-3.0-or-later
// nxnative's nn::fs (hle_fs.c), ported: reading the game's RomFS, and a save written, committed and read back. The
// commit is the transactional one: the work tree is cloned (reflinks on btrfs) and swapped in atomically.
#include "nx.h"
typedef struct { u64 v[2]; } Uid;
Result fs_mount_rom(const char *name, void *cache, u64 size) __asm__("_ZN2nn2fs8MountRomEPKcPvm");
Result fs_open(u64 *h, const char *path, int mode) __asm__("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
Result fs_read(u64 *n, u64 h, s64 off, void *buf, u64 size) __asm__("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm");
Result fs_size(s64 *out, u64 h) __asm__("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE");
void fs_close(u64 h) __asm__("_ZN2nn2fs9CloseFileENS0_10FileHandleE");
Result fs_mount_save(const char *name, const Uid *uid) __asm__("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE");
Result fs_create(const char *path, s64 size) __asm__("_ZN2nn2fs10CreateFileEPKcl");
Result fs_write(u64 h, s64 off, const void *buf, u64 size, const int *opt) __asm__("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE");
Result fs_commit(const char *name) __asm__("_ZN2nn2fs6CommitEPKc");
enum { READ = 1, WRITE = 2 };

static int same(const char *a, const char *b, int n) { for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0; return 1; }

int test_main(void) {
    CHECK("MountRom", fs_mount_rom("rom", NULL, 0) == 0);
    u64 h = 0;
    CHECK("open a RomFS file", fs_open(&h, "rom:/data/hello.txt", READ) == 0);
    s64 size = -1;
    CHECK("its size", fs_size(&size, h) == 0 && size == 21);
    char buf[64] = {0};
    u64 got = 0;
    CHECK("read it", fs_read(&got, h, 0, buf, sizeof buf) == 0 && got == 21 && same(buf, "hello from the romfs\n", 21));
    CHECK("read from an offset", fs_read(&got, h, 6, buf, 4) == 0 && got == 4 && same(buf, "from", 4));
    fs_close(h);
    CHECK("a missing file is an error", fs_open(&h, "rom:/data/nothing.bin", READ) != 0);

    Uid user = {{0x78626c0000000001ul, 0x53574e4154495645ul}};
    CHECK("MountSaveData", fs_mount_save("save", &user) == 0);
    CHECK("create a save file", fs_create("save:/progress.bin", 16) == 0);
    CHECK("open it for writing", fs_open(&h, "save:/progress.bin", WRITE) == 0);
    int opt = 1;   // WriteOption::Flush
    CHECK("write it", fs_write(h, 0, "WORLD 3-2 CLEAR!", 16, &opt) == 0);
    fs_close(h);
    CHECK("Commit (clone and atomic swap)", fs_commit("save") == 0);
    CHECK("read the save back", fs_open(&h, "save:/progress.bin", READ) == 0 && fs_read(&got, h, 0, buf, 16) == 0 && got == 16 && same(buf, "WORLD 3-2 CLEAR!", 16));
    fs_close(h);
    return 0;
}
