// SPDX-License-Identifier: GPL-3.0-or-later
// What the program asks the kernel about itself: from its NPDM (main.npdm in the exefs: program ID, main thread priority
// and stack size, system resource size) and, when one exists for its title, nxnative's game profile
// (dm/games/<title ID>/game.tsv), whose memory keys are what nxnative's host reports through svcGetInfo. A profile's
// values win over the NPDM's, and either over the defaults.
#include "nxl.h"
#include "platform.h"
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

nxl_profile g_profile = {
    .program_id = 0x0100000000001000ull,
    .mem_alias = 0x1000000000ull,      // Horizon's 39-bit alias region: 64 GiB
    .mem_stack = 0x0080000000ull,
    .app_pool = 0xCD500000ull,         // a retail application's pool: about 3.2 GiB
    .mem_used = 0x4000000ull,          // nxnative's default (game.py mem_used)
    .sys_resource = 0x2000000ull,      // nxnative's default (game.py sys_resource): virtual address memory enabled
    .main_prio = 44,
    .main_stack = 16u << 20,           // without an NPDM (homebrew, tests); a game's NPDM gives its own
};

// Reads a whole file.
static int read_all(const char *path, uint8_t **out, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    struct stat st;
    fstat(fileno(f), &st);
    *out = malloc(st.st_size + 1);
    *n = fread(*out, 1, st.st_size, f);
    (*out)[*n] = 0;
    fclose(f);
    return 0;
}

// META header: +0xE main thread priority, +0xF its core, +0x14 system resource size, +0x1C main thread stack size,
// +0x20 name, +0x70 / +0x74 the ACI0 section (program ID at +0x10)
static void read_npdm(const char *exefs) {
    char p[4096];
    snprintf(p, sizeof p, "%s/main.npdm", exefs);
    uint8_t *b;
    size_t n;
    if (read_all(p, &b, &n)) return;
    if (n >= 0x80 && !memcmp(b, "META", 4)) {
        uint32_t aci = *(uint32_t *)(b + 0x70), acisz = *(uint32_t *)(b + 0x74);
        g_profile.main_prio = b[0xE];
        g_profile.sys_resource = *(uint32_t *)(b + 0x14);
        if (*(uint32_t *)(b + 0x1C)) g_profile.main_stack = *(uint32_t *)(b + 0x1C);
        if ((uint64_t)aci + 0x18 <= n && acisz >= 0x18 && !memcmp(b + aci, "ACI0", 4)) g_profile.program_id = *(uint64_t *)(b + aci + 0x10);
        snprintf(g_profile.name, sizeof g_profile.name, "%.16s", (const char *)b + 0x20);
        g_profile.have_npdm = 1;
        LOG("npdm: program %016llx \"%s\", main thread priority %d, stack %#x, system resource %#llx",
            (unsigned long long)g_profile.program_id, g_profile.name, g_profile.main_prio, g_profile.main_stack,
            (unsigned long long)g_profile.sys_resource);
    } else RED("%s is not an NPDM", p);
    free(b);
}

// Reads a number (decimal or 0x hex); 0 when it read whole.
static int parse_u64(const char *v, uint64_t *out) {
    char *e;
    errno = 0;
    unsigned long long x = strtoull(v, &e, 0);
    if (errno || e == v) return -1;
    *out = x;
    return 0;
}

// Reads a game profile (game.tsv: name, value lines): its memory sizes and other settings.
static void read_tsv(const char *path) {
    uint8_t *b;
    size_t n;
    if (read_all(path, &b, &n)) return;
    g_profile.mem_used = 0x4000000, g_profile.sys_resource = 0x2000000;   // game.py's defaults when the keys are absent
    char *save = NULL;
    for (char *line = strtok_r((char *)b, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (line[0] == '#' || !line[0]) continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char *k = line, *v = tab + 1;
        uint64_t x;
        if (!strcmp(k, "name")) snprintf(g_profile.name, sizeof g_profile.name, "%s", v);
        else if (!strcmp(k, "mem_map_limit")) g_profile.mem_map_limit = !strcmp(v, "on");
        else if (parse_u64(v, &x)) continue;
        else if (!strcmp(k, "program_id")) g_profile.program_id = x;
        else if (!strcmp(k, "mem_alias")) g_profile.mem_alias = x;
        else if (!strcmp(k, "mem_stack")) g_profile.mem_stack = x;
        else if (!strcmp(k, "mem_reserve")) g_profile.mem_reserve = x;
        else if (!strcmp(k, "app_pool")) g_profile.app_pool = x;
        else if (!strcmp(k, "mem_used")) g_profile.mem_used = x;
        else if (!strcmp(k, "sys_resource")) g_profile.sys_resource = x;
    }
    free(b);
    snprintf(g_profile.path, sizeof g_profile.path, "%s", path);
    g_profile.have_profile = 1;
}

// The game's profile: its NPDM's values, then a game.tsv (given, or nxnative's for its program ID) over them.
void profile_load(const char *exefs, const char *profile_arg) {
    if (exefs) read_npdm(exefs);
    char p[4096];
    if (profile_arg) {   // a game.tsv, or a profile folder
        struct stat st;
        if (!stat(profile_arg, &st) && S_ISDIR(st.st_mode)) snprintf(p, sizeof p, "%s/game.tsv", profile_arg);
        else snprintf(p, sizeof p, "%s", profile_arg);
        if (access(p, R_OK)) die("no game profile at %s", p);
        read_tsv(p);
    } else if (g_profile.have_npdm) {   // nxnative's profile of this title, if it has one
        char inst[4096] = "", beside[4096] = "";   // an install's copy (<prefix>/share/nxlinux/games), the zip's (games\ beside it)
        if (!nxl_exe_dir(inst, sizeof inst - 32)) snprintf(beside, sizeof beside, "%s/games", inst), strcat(inst, "/../share/nxlinux/games");
        const char *dirs[4] = {getenv("NXL_GAMES_DIR"), NXL_DEFAULT_GAMES_DIR, inst, beside};
        for (int i = 0; i < 4 && !g_profile.have_profile; i++) {
            if (!dirs[i] || !*dirs[i]) continue;
            snprintf(p, sizeof p, "%s/%016llX/game.tsv", dirs[i], (unsigned long long)g_profile.program_id);
            if (!access(p, R_OK)) read_tsv(p);
        }
    }
    if (g_profile.have_profile)
        LOG("game profile %s (%s): alias %#llx, stack %#llx, application pool %#llx, system resource %#llx", g_profile.path,
            g_profile.name, (unsigned long long)g_profile.mem_alias, (unsigned long long)g_profile.mem_stack,
            (unsigned long long)g_profile.app_pool, (unsigned long long)g_profile.sys_resource);
    else if (g_profile.have_npdm) LOG("no nxnative game profile for %016llx: default memory layout", (unsigned long long)g_profile.program_id);
    g_program_id = g_profile.program_id;
}
