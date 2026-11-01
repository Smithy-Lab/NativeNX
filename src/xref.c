// SPDX-License-Identifier: GPL-3.0-or-later
// Looking into a game's own code without sharing it: --xref finds every place the loaded, linked modules refer to an
// address (a global variable, or the GOT slot holding its address), and marks the ones that store to it; --dump prints
// code words. The output is plain text a person can paste: addresses, symbols and instruction words, no game data.
#include "nxl.h"

// "main+0x227df98" or "0x80227df98"
static uint64_t parse_addr(const char *s) {
    const char *plus = strchr(s, '+');
    if (!plus) return strtoull(s, NULL, 0);
    for (int i = 0; i < g_nmodules; i++)
        if (strlen(g_modules[i]->name) == (size_t)(plus - s) && !strncmp(g_modules[i]->name, s, (size_t)(plus - s)))
            return (uint64_t)(uintptr_t)g_modules[i]->base + strtoull(plus + 1, NULL, 0);
    die("--xref/--dump: no module named %.*s", (int)(plus - s), s);
}
// An address described: its module, symbol and offset.
static void where(uint64_t a, char *out, size_t n) {
    uint64_t off = 0;
    const char *mod = NULL, *sym = module_symbolize(a, &off, &mod);
    module_t *m = module_of(a);
    if (!m) { snprintf(out, n, "%#llx", (unsigned long long)a); return; }
    int code = a >= (uint64_t)(uintptr_t)m->base + m->text_off && a < (uint64_t)(uintptr_t)m->base + m->text_off + m->text_size;
    if (sym && code) snprintf(out, n, "%s+%#llx (%s+%#llx)", m->name, (unsigned long long)(a - (uint64_t)(uintptr_t)m->base), sym, (unsigned long long)off);
    else snprintf(out, n, "%s+%#llx", m->name, (unsigned long long)(a - (uint64_t)(uintptr_t)m->base));
}
// Prints the instruction words around an address (for --dump).
static void words(uint64_t a, int n) {
    module_t *m = module_of(a);
    if (!m) return;
    uint64_t lo = (uint64_t)(uintptr_t)m->base + m->text_off, hi = lo + m->text_size;
    if (a < lo) a = lo;
    for (int k = 0; k < n && a + 4 * k < hi; k += 8) {
        printf("    %s+%07llx:", m->name, (unsigned long long)(a + 4 * k - (uint64_t)(uintptr_t)m->base));
        for (int j = k; j < k + 8 && j < n && a + 4 * j < hi; j++) printf(" %08x", ((const uint32_t *)(uintptr_t)a)[j]);
        putchar('\n');
    }
}

// --dump: prints the code at a guest address and what calls it.
void nxl_dump(const char *spec, int n) {
    uint64_t a = parse_addr(spec) & ~3ull;
    char w[256];
    where(a, w, sizeof w);
    printf("code at %s, %d words:\n", w, n);
    words(a, n);
}

// adrp pages per register as the scan goes; a reference is an add / load / store off such a page
void nxl_xref(const char *spec) {
    uint64_t slot = parse_addr(spec), var = 0;
    module_t *sm = module_of(slot);
    if (sm && slot >= (uint64_t)(uintptr_t)sm->base + sm->ro_off) var = *(const uint64_t *)(uintptr_t)slot;   // a GOT slot or pointer: what it holds
    char w[256];
    where(slot, w, sizeof w);
    printf("references to %s", w);
    if (var) { char v[256]; where(var, v, sizeof v); printf(" and to what it holds, %s", v); }
    printf(":\n");
    int hits = 0;
    for (int i = 0; i < g_nmodules; i++) {
        module_t *m = g_modules[i];
        uint64_t lo = (uint64_t)(uintptr_t)m->base + m->text_off;
        const uint32_t *t = (const uint32_t *)(uintptr_t)lo;
        uint64_t n = m->text_size / 4;
        uint64_t page[32];
        uint64_t page_at[32];
        memset(page_at, 0, sizeof page_at);
        for (uint64_t k = 0; k < n; k++) {
            uint32_t x = t[k];
            uint64_t pc = lo + 4 * k;
            if ((x & 0x9F000000) == 0x90000000) {   // adrp
                int64_t imm = (int64_t)((((uint64_t)x >> 29) & 3) | (((uint64_t)x >> 3) & 0x1FFFFC));
                if (imm & 0x100000) imm -= 0x200000;
                page[x & 31] = (pc & ~0xFFFull) + (uint64_t)(imm << 12), page_at[x & 31] = k + 1;
                continue;
            }
            int rn = (x >> 5) & 31;
            if (!page_at[rn] || k - (page_at[rn] - 1) > 24) continue;   // an adrp of this register shortly before
            uint64_t ref = 0;
            const char *kind = NULL;
            if ((x & 0xFF800000) == 0x91000000) ref = page[rn] + ((x >> 10) & 0xFFF), kind = "address";   // add (64-bit, imm)
            else if ((x & 0x3B000000) == 0x39000000) {   // ldr / str (unsigned offset), any size
                unsigned size = x >> 30, opc = (x >> 22) & 3;
                ref = page[rn] + (((x >> 10) & 0xFFF) << size), kind = opc == 0 ? "store" : "load";
            }
            if (!kind || (ref != slot && (!var || ref != var))) continue;
            char at[256];
            where(pc, at, sizeof at);
            const char *what = ref == slot ? "the slot" : "the variable";
            // through the slot: a store to [xd] or [xd, #imm] in the next instructions writes the variable
            int writes = !strcmp(kind, "store");
            if (ref == slot && !strcmp(kind, "load")) {
                int rd = x & 31;
                for (uint64_t j = k + 1; j < n && j < k + 16; j++) {
                    uint32_t y = t[j];
                    if ((y & 0x3B000000) == 0x39000000 && ((y >> 22) & 3) == 0 && (int)((y >> 5) & 31) == rd) { writes = 1; break; }
                    if ((y & 31) == (uint32_t)rd && (y & 0x3B000000) != 0x39000000) break;   // rd overwritten (roughly)
                }
            }
            printf("  %s: %s %s%s\n", at, kind, what, writes ? "  <-- WRITES" : "");
            words(pc - 16, 16);
            if (++hits >= 200) { printf("  (stopping at 200)\n"); return; }
        }
    }
    printf("%d references\n", hits);
}
