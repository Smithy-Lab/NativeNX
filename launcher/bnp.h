// SPDX-License-Identifier: GPL-3.0-or-later
// BCML mods for Breath of the Wild (.bnp, unpacked into the game's mods folder). BCML, BotW's mod manager, packs a mod
// for installing rather than for playing: its archives (SARC packs: .sbactorpack, .pack ...) hold only the files the
// mod changes, and its logs say what to merge into the game. NativeNX does that merge for the mods turned on (this
// file), into a folder the game's file service lays over the game's files like any mod:
//   · each pack logs/packs.json names: the game's own pack, with the mods' files laid over it (packs inside packs too);
//     Yaz0-compressed ones (an "s" before the extension) stay so (stored, not recompressed);
//   · the resource size table (System/Resource/ResourceSizeTable.product.srsizetable): the game's, with the sizes in
//     logs/rstb.json (a size 0 removes the entry); BotW crashes on a resource larger than its entry;
//   · the other files: the mod's own;
//   · the mod's options (options/<folder>/, as info.json lists them): their files, packs and sizes over the mod's;
//   · logs/deepmerge.aamp and aslist.aamp: BCML leaves the game's parameter files a mod changes (AAMP, aamp.h) out of
//     its packs and logs the changes; they are merged into those files, wherever they are in the packs;
//   · the size table as BCML's RstbMerger writes it: the logged sizes and those of the files made or brought, worked
//     out as BCML does (botw rstb's sizes per resource type, botw-utils' estimates), none below the game's own.
//   · the DLC's files (a mod's 01007EF00011F000/romfs, and logged changes to "Aoc/..." files): merged the same way over
//     the DLC's own, into a folder laid over the DLC (names "Aoc/0010/..." in the size table).
// Not done: game data, save data and map merges (other logs).
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct { char name[96], desc[256], folder[128]; int def; } bnp_opt_t;
typedef struct {
    char dir[1100];          // the mod's folder (info.json, logs/, <program ID>/romfs, options/)
    char romfs[1100];        // its own files
    char aoc[1100];          // its DLC files (<DLC ID>/romfs: 01007EF00011F000/romfs), or ""
    char name[128], version[32];
    bnp_opt_t opt[16]; int nopt;
} bnp_t;
// a BCML mod's folder: 1 (with *b filled) when dir holds one (info.json and logs/)
int bnp_detect(const char *dir, bnp_t *b);

// the game's own files: path in its RomFS ("Actor/Pack/X.sbactorpack"), malloc'd into *data; 0 when found
typedef int (*bnp_reader)(void *ctx, const char *path, uint8_t **data, size_t *n);
// the merge of the mods (lowest priority first; on[i]: a bit per option, 1 on) into out/romfs, and their DLC files into
// out/aoc (read with rd_aoc from the DLC; NULL: no DLC, those left out); 0 on success
int bnp_build(const bnp_t *mods, const uint32_t *on, int n, bnp_reader rd, void *ctx, bnp_reader rd_aoc, void *ctx_aoc, const char *out, char *err, size_t en);

// the formats (bnp.c; the tests use them too)
int yaz0_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *on);   // 0 done
uint8_t *yaz0_store(const uint8_t *in, size_t n, const uint8_t *hdr, size_t *on);   // Yaz0 without compression (hdr: the 16-byte header to keep, or NULL)
typedef struct { char *name; const uint8_t *data; size_t size; uint32_t align; uint8_t *own; } sarc_ent;
int sarc_read(const uint8_t *in, size_t n, sarc_ent **e);   // entries (pointing into in), their count; -1 not a SARC
uint8_t *sarc_write(sarc_ent *e, int n, size_t *on);         // a SARC of the entries (sorted by name hash)
void sarc_free(sarc_ent *e, int n);
int rstb_set(uint8_t **tab, size_t *n, const char *name, uint32_t size);   // in a decompressed RSTB: set (0: remove) an entry
int rstb_get(const uint8_t *tab, size_t n, const char *name, uint32_t *size);   // 1 found
