// SPDX-License-Identifier: GPL-3.0-or-later
// What a game draws with (library.h gfx_api): its code modules' imports, read from the NSOs as nxlinux's loader reads
// them (src/loader.c loader_gfx_api). An NSO's read-only and data segments (LZ4-compressed or not) hold the dynamic
// section (found through the MOD0 header) and its symbol and string tables; the undefined symbols are the imports. A
// GPU driver of the game's (a module that defines EGL's or Vulkan's entry points, not main) is not counted.
#define _GNU_SOURCE
#include "library.h"
#include "lz4.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t u32at(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t u64at(const uint8_t *p) { return u32at(p) | (uint64_t)u32at(p + 4) << 32; }

static int api_of(const char *n) {   // as loader.c's
    if (!strncmp(n, "nvn", 3) && n[3] >= 'A' && n[3] <= 'Z') return 0;
    if ((!strncmp(n, "egl", 3) && n[3] >= 'A' && n[3] <= 'Z') || (!strncmp(n, "gl", 2) && n[2] >= 'A' && n[2] <= 'Z')) return 1;
    if (!strncmp(n, "vk", 2) && n[2] >= 'A' && n[2] <= 'Z') return 2;
    return -1;
}

// one module's imports of each API added to cnt (unless it is a GPU driver); 0 when it is not an NSO this can read
static int nso_count(const char *path, unsigned cnt[3]) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 0x100 || fsz > (1L << 30)) { fclose(f); return 0; }
    uint8_t *file = malloc((size_t)fsz);
    int ok = file && fread(file, 1, (size_t)fsz, f) == (size_t)fsz;
    fclose(f);
    if (!ok || memcmp(file, "NSO0", 4)) { free(file); return 0; }
    uint32_t flags = u32at(file + 0xC), off[3], mem[3], size[3], csize[3];
    for (int i = 0; i < 3; i++) off[i] = u32at(file + 0x10 + 0x10 * i), mem[i] = u32at(file + 0x14 + 0x10 * i), size[i] = u32at(file + 0x18 + 0x10 * i),
                                csize[i] = u32at(file + 0x60 + 4 * i);
    uint64_t total = (uint64_t)mem[2] + size[2];
    uint8_t *img = total && total < (1ull << 31) ? calloc(1, total) : NULL;
    ok = img != NULL;
    for (int i = 0; ok && i < 3; i++) {   // all three: MOD0's offset is in the first words of text
        uint32_t cs = (flags >> i & 1) ? csize[i] : size[i];
        if ((uint64_t)off[i] + cs > (uint64_t)fsz || (uint64_t)mem[i] + size[i] > total) { ok = 0; break; }
        if (flags >> i & 1) ok = LZ4_decompress_safe((const char *)file + off[i], (char *)img + mem[i], (int)cs, (int)size[i]) == (int)size[i];
        else memcpy(img + mem[i], file + off[i], size[i]);
    }
    free(file);
    uint32_t mod0 = ok && total >= 8 ? u32at(img + 4) : 0;
    if (!ok || !mod0 || (uint64_t)mod0 + 8 > total || memcmp(img + mod0, "MOD0", 4)) { free(img); return 0; }
    uint64_t dyn = mod0 + (uint64_t)(int32_t)u32at(img + mod0 + 4), symtab = 0, strtab = 0, strsz = 0, hash = 0;
    for (uint64_t p = dyn; p + 16 <= total; p += 16) {
        uint64_t tag = u64at(img + p), val = u64at(img + p + 8);
        if (!tag) break;
        if (tag == 6) symtab = val; else if (tag == 5) strtab = val; else if (tag == 10) strsz = val; else if (tag == 4) hash = val;
    }
    if (!symtab || !strtab || strtab + strsz > total) { free(img); return 1; }
    uint64_t nsyms = hash && hash + 8 <= total ? u32at(img + hash + 4) : strtab > symtab ? (strtab - symtab) / 24 : 0;
    unsigned c[3] = {0};
    int driver = 0, has_main = 0;
    for (uint64_t i = 1; i < nsyms && symtab + 24 * (i + 1) <= total; i++) {
        const uint8_t *s = img + symtab + 24 * i;
        uint32_t name = u32at(s);
        if (!name || name >= strsz) continue;
        const char *n = (const char *)img + strtab + name;
        uint16_t shndx = (uint16_t)(s[6] | s[7] << 8);
        if (shndx) {   // defined here
            if (!strcmp(n, "nnMain")) has_main = 1;
            if (!strcmp(n, "eglGetDisplay") || !strcmp(n, "vkCreateInstance") || !strcmp(n, "NvGlEs2Init") || !strcmp(n, "vk_icdGetInstanceProcAddr")) driver = 1;
            continue;
        }
        int a = api_of(n);
        if (a >= 0) c[a]++;
    }
    if (!driver || has_main) for (int i = 0; i < 3; i++) cnt[i] += c[i];
    free(img);
    return 1;
}

// What a game draws with (NVN, OpenGL or Vulkan), from the imports of its code modules.
int gfx_api(const char *exefs) {
    static const char *const mods[] = {"main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk"};
    unsigned cnt[3] = {0};
    int any = 0;
    for (size_t i = 0; i < sizeof mods / sizeof *mods; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", exefs, mods[i]);
        any |= nso_count(p, cnt);
    }
    if (!any) return GFX_UNKNOWN;
    int b = cnt[0] >= cnt[1] && cnt[0] >= cnt[2] ? 0 : cnt[1] >= cnt[2] ? 1 : 2;   // as loader.c: the most imports, NVN wins a tie
    return !cnt[b] ? GFX_UNKNOWN : b == 0 ? GFX_NVN : b == 1 ? GFX_OPENGL : GFX_VULKAN;
}
const char *gfx_api_name(int api) {
    return api == GFX_NVN ? "NVN" : api == GFX_OPENGL ? "OpenGL" : api == GFX_VULKAN ? "Vulkan" : "unknown";
}
