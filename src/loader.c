// SPDX-License-Identifier: GPL-3.0-or-later
// NSO loading and dynamic linking. A Switch program is a set of NSO modules (main, subsdk0-9, sdk; rtld is not needed:
// this loader does its work). Each is mapped at its guest address in the ASLR region, then every module's relocations
// are applied once all are mapped, so imports can bind across modules.
// Symbol binding follows nxnative (dm/host/loader.c): a forced host implementation (HLE_FORCE) wins over any module's
// definition; then the modules' own definitions in load order; then a plain host implementation (HLE); an import that
// none provide is bound to a stub that reports the call, and a weak one to 0.
#include "nxl.h"
#include <elf.h>
#include <errno.h>
#include <sys/stat.h>
#include "lz4.h"

module_t *g_modules[NXL_MAX_MODULES];
int g_nmodules;
static int g_nmissing;
void mem_protect(uint64_t a, uint64_t s, int writable, uint32_t type, uint32_t perm);

typedef struct __attribute__((packed)) {
    char magic[4];
    uint32_t version, reserved, flags;
    struct { uint32_t file_off, mem_off, size, extra; } seg[3];   // text (extra: name offset), ro (name size), data (bss size)
    uint8_t build_id[32];
    uint32_t csize[3];
    uint8_t reserved2[0x1C];
    uint32_t api_info[2], dynstr[2], dynsym[2];
    uint8_t hash[3][32];
} nso_header;
_Static_assert(sizeof(nso_header) == 0x100, "NSO header size");

// Reads a whole file (stops the run when it cannot).
static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) die("%s: %s", path, strerror(errno));
    struct stat st;
    fstat(fileno(f), &st);
    uint8_t *b = malloc(st.st_size ? st.st_size : 1);
    if (fread(b, 1, st.st_size, f) != (size_t)st.st_size) die("%s: short read", path);
    fclose(f);
    *size = st.st_size;
    return b;
}

static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }

// Finds a module's dynamic section (by its MOD0 header) and reads its symbol and string tables and a hash table of its
// symbols.
static void parse_dynamic(module_t *m) {
    const uint8_t *text = m->base + m->text_off;
    uint32_t mod0 = *(const uint32_t *)(text + 4);
    if (mod0 + 28 > m->size || memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0 header", m->name);
    const int32_t *h = (const int32_t *)(m->base + mod0);
    m->dynamic = (const uint64_t *)(m->base + mod0 + h[1]);
    uint64_t hash = 0, gnu_hash = 0;
    for (const Elf64_Dyn *d = (const Elf64_Dyn *)m->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = m->base + d->d_un.d_ptr; break;
        case DT_STRTAB: m->strtab = (const char *)m->base + d->d_un.d_ptr; break;
        case DT_HASH: hash = d->d_un.d_ptr; break;
        case DT_GNU_HASH: gnu_hash = d->d_un.d_ptr; break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
        }
    }
    if (hash) m->nsyms = ((const uint32_t *)(m->base + hash))[1];   // nchain
    else if (gnu_hash) {   // the highest symbol index any bucket chain reaches, plus one
        const uint32_t *g = (const uint32_t *)(m->base + gnu_hash);
        uint32_t nbuckets = g[0], symoff = g[1], bloom = g[2];
        const uint32_t *buckets = (const uint32_t *)((const uint8_t *)(g + 4) + bloom * 8), *chain = buckets + nbuckets;
        uint32_t last = 0;
        for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
        if (last) while (!(chain[last - symoff] & 1)) last++;
        m->nsyms = last ? last + 1 : symoff;
    } else if (m->symtab && m->strtab && (const uint8_t *)m->strtab > (const uint8_t *)m->symtab)
        m->nsyms = ((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym);
    // a hash of the defined symbols, for lookups across modules
    uint32_t cap = 16;
    while (cap < m->nsyms * 2) cap <<= 1;
    m->symhash = calloc(cap, sizeof *m->symhash);
    m->symhash_mask = cap - 1;
    const Elf64_Sym *st = m->symtab;
    for (uint32_t i = 1; st && m->strtab && i < m->nsyms; i++) {
        if (st[i].st_shndx == SHN_UNDEF || !st[i].st_name) continue;
        int bind = ELF64_ST_BIND(st[i].st_info);
        if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
        uint32_t j = sym_hash(m->strtab + st[i].st_name) & m->symhash_mask;
        while (m->symhash[j]) j = (j + 1) & m->symhash_mask;
        m->symhash[j] = i;
    }
}

// A symbol of a module by name (its hash table).
static const Elf64_Sym *module_find(module_t *m, const char *name) {
    const Elf64_Sym *st = m->symtab;
    for (uint32_t j = sym_hash(name) & m->symhash_mask; m->symhash[j]; j = (j + 1) & m->symhash_mask)
        if (!strcmp(m->strtab + st[m->symhash[j]].st_name, name)) return &st[m->symhash[j]];
    return NULL;
}

// The address of a symbol defined in any loaded module, or 0.
uint64_t guest_sym(const char *name) {
    for (int i = 0; i < g_nmodules; i++) {
        const Elf64_Sym *s = module_find(g_modules[i], name);
        if (s) return (uint64_t)g_modules[i]->base + s->st_value;
    }
    return 0;
}

// Whether a symbol of the loaded modules is a function.
int guest_sym_is_func(const char *name) {
    for (int i = 0; i < g_nmodules; i++) {
        const Elf64_Sym *s = module_find(g_modules[i], name);
        if (s) return ELF64_ST_TYPE(s->st_info) == STT_FUNC;
    }
    return 0;
}

module_t *module_of(uint64_t addr) {
    for (int i = 0; i < g_nmodules; i++)
        if (addr - (uint64_t)g_modules[i]->base < g_modules[i]->size) return g_modules[i];
    return NULL;
}

// nxnative's registered cuts of particular module builds (tools/gen_cuts.py → nxl_cuts.c): the few instructions it changes in
// a known SDK or game build so that it fits a host's memory (Super Mario Bros. Wonder's SDK 16.2.0: the AddressRegionManager's
// 1 GB node area and its 1 GB region alignment, which leave the game's 2.875 GB alias region without room). Every original
// byte is checked first and a build's cuts go in all or none, as in nxnative's patch_nso.py
typedef struct { const char *build; uint32_t off, len; const uint8_t *orig, *repl; const char *why; } nxl_cut;
extern const nxl_cut nxl_cuts[];
extern const size_t nxl_ncuts;
static int apply_cuts(module_t *m) {   // 1 when the build's cuts are in (or it has none), 0 when they are not
    char bid[33];
    for (int i = 0; i < 16; i++) snprintf(bid + 2 * i, 3, "%02x", m->build_id[i]);
    size_t first = nxl_ncuts, n = 0;
    for (size_t i = 0; i < nxl_ncuts; i++)
        if (!strcmp(nxl_cuts[i].build, bid)) { if (first == nxl_ncuts) first = i; n++; }
    if (!n) return 1;
    // NXL_NO_CUTS=1: none; NXL_NO_CUTS=main,subsdk0: not those modules' (Breath of the Wild 1.9.0's main cuts lift its frame
    // rate floor to 60 fps for nxnative's frame step, which nxlinux lacks: without it the game ran at 42 fps believing it
    // ran at 60, its time at 70% of real time and its sound ahead of it; NXL_NO_CUTS=main keeps its own 30 fps rule)
    const char *nc = getenv("NXL_NO_CUTS");
    int skip = 0;
    if (nc && *nc) {
        if (!strcmp(nc, "1")) skip = 1;
        else {
            size_t ln = strlen(m->name);
            for (const char *p = nc; *p;) {
                const char *e = strchr(p, ',');
                size_t len = e ? (size_t)(e - p) : strlen(p);
                if (len == ln && !strncmp(p, m->name, ln)) skip = 1;
                p += len + (e ? 1 : 0);
            }
        }
    }
    if (skip) { LOG("%s (build %s): %zu registered cuts not applied (NXL_NO_CUTS)", m->name, bid, n); return 0; }
    for (size_t i = first; i < first + n; i++) {
        const nxl_cut *c = &nxl_cuts[i];
        if ((uint64_t)c->off + c->len > m->size || memcmp(m->base + c->off, c->orig, c->len)) {
            RED("%s (build %s): the original bytes of the cut at +%#x do not match: none of its %zu cuts applied", m->name, bid, c->off, n);
            return 0;
        }
    }
    for (size_t i = first; i < first + n; i++) {
        memcpy(m->base + nxl_cuts[i].off, nxl_cuts[i].repl, nxl_cuts[i].len);
        if (nxl_verbose > 1) nxl_log("[nxl] ", "%s+%#x cut: %s", m->name, nxl_cuts[i].off, nxl_cuts[i].why);
    }
    LOG("%s (build %s): applied nxnative's %zu registered cuts", m->name, bid, n);
    return 1;
}

// nxnative's frame step (tools/gen_fstep.py → nxl_fstep.c, from FRAME_STEP of its games' patches.py; Breath of the Wild 1.9.0,
// Tears of the Kingdom 1.4.3, Hyrule Warriors: Age of Calamity, Paper Mario: The Thousand-Year Door, ASTRAL CHAIN). The games
// step their game time by a whole number of vsyncs per frame that their own interval logic picks; the stubs hand them the step
// the host decides instead (nxnative's hle_nvn.c fstep_tick: 1 vsync = 60 fps while frames hold it, 2 = 30 when they do not),
// and presents follow it, so game time stays real time at either rate. nxnative assembles the stubs into a patch area inside
// its rewritten image; here the patch area and the state page (layout: nxnative's patch_nso.py FRAME_STEP; +0 float target
// fps, +0x10 float render scale, +0x20 the host's step) are mapped after the image, and the stubs' branches and ADRPs, made
// for placeholders P0 / S0, are moved to them. The state page's settings come from DM_FPS_TARGET (0 = the game's own frame
// step, 30 or 60) and DM_RENDER_SCALE (0.5 to 1.0), as in nxnative's loader
typedef struct { uint32_t off, len; const uint8_t *orig; } nxl_fstep_site;
typedef struct { uint32_t off, n; const uint32_t *w; const char *why; } nxl_fstep_put;
typedef struct { const char *build; uint32_t p0, s0, nwords; const uint32_t *words; int render_scale;
                 uint32_t nsites; const nxl_fstep_site *sites; uint32_t nputs; const nxl_fstep_put *puts; } nxl_fstep;
extern const nxl_fstep nxl_fsteps[];
extern const size_t nxl_nfsteps;
uint8_t *dm_fps_state;   // read by nxnative's hle_nvn.c (a weak symbol there); NULL when no module has frame step stubs
#define FS_STATE_SIZE 0x4000ull
int nxl_fstep_reloc(uint64_t p0, uint64_t s0, uint64_t p, uint64_t st, uint64_t at0, uint64_t at, uint32_t w, uint32_t *out, uint64_t *branch_to);
static const nxl_fstep *fstep_of(const uint8_t *build_id) {
    char bid[33];
    for (int i = 0; i < 16; i++) snprintf(bid + 2 * i, 3, "%02x", build_id[i]);
    for (size_t i = 0; i < nxl_nfsteps; i++) if (!strcmp(nxl_fsteps[i].build, bid)) return &nxl_fsteps[i];
    return NULL;
}
// One word of the stubs or of a site (fstep.c): a B / BL or ADRP moved to where its target went; 0 when out of reach
static int fs_reloc(const nxl_fstep *f, const module_t *m, uint64_t at0, uint64_t at, uint32_t w, uint32_t *out, uint64_t *branch_to) {
    uint64_t t = 0;
    if (!nxl_fstep_reloc(f->p0, f->s0, m->fs_off, m->fs_state_off, at0, at, w, out, &t)) return 0;
    *branch_to = t ? (uint64_t)m->base + t : 0;
    return 1;
}
static void fs_hint(module_t *m, uint64_t a) {
    if (!a) return;
    m->fs_hints = realloc(m->fs_hints, (m->fs_nhints + 1) * sizeof *m->fs_hints);
    m->fs_hints[m->fs_nhints++] = a;
}
// Puts a game's frame step stubs into its code (when the bytes there are the ones they were made for), for 60 fps.
static void install_fstep(module_t *m, const nxl_fstep *f) {
    uint8_t *b = m->base;
    for (uint32_t i = 0; i < f->nsites; i++) {
        const nxl_fstep_site *s = &f->sites[i];
        if ((uint64_t)s->off + s->len > m->text_off + m->text_size || memcmp(b + s->off, s->orig, s->len)) {
            RED("%s: the original bytes of the frame step site at +%#x do not match: frame step not installed", m->name, s->off);
            m->fs_size = 0; return;
        }
    }
    uint32_t *stubs = (uint32_t *)(b + m->fs_off);
    uint64_t to;
    for (uint32_t i = 0; i < f->nwords; i++) {
        if (!fs_reloc(f, m, f->p0 + 4ull * i, m->fs_off + 4ull * i, f->words[i], &stubs[i], &to)) {
            RED("%s: a frame step stub's branch does not reach: frame step not installed", m->name);
            memset(stubs, 0, 4ull * f->nwords); m->fs_size = 0; return;
        }
        fs_hint(m, to);
    }
    uint32_t put[64];
    for (uint32_t i = 0; i < f->nputs; i++) {
        const nxl_fstep_put *p = &f->puts[i];
        for (uint32_t k = 0; k < p->n && k < 64; k++)
            if (!fs_reloc(f, m, p->off + 4ull * k, p->off + 4ull * k, p->w[k], &put[k], &to)) {
                RED("%s: a frame step site's branch does not reach: frame step not installed", m->name);
                memset(stubs, 0, 4ull * f->nwords); m->fs_size = 0; return;
            }
    }
    for (uint32_t i = 0; i < f->nputs; i++) {   // all checked: now written
        const nxl_fstep_put *p = &f->puts[i];
        for (uint32_t k = 0; k < p->n && k < 64; k++) {
            fs_reloc(f, m, p->off + 4ull * k, p->off + 4ull * k, p->w[k], &put[k], &to);
            memcpy(b + p->off + 4ull * k, &put[k], 4);
            fs_hint(m, to);
        }
        fs_hint(m, (uint64_t)b + p->off + 4ull * p->n);
        if (nxl_verbose > 1) nxl_log("[nxl] ", "%s+%#x frame step: %s", m->name, p->off, p->why);
    }
    dm_fps_state = b + m->fs_state_off;
    const char *e = getenv("DM_FPS_TARGET");
    int t = e ? atoi(e) : 0;
    if (t != 0 && t != 30 && t != 60) { RED("DM_FPS_TARGET=%s: must be 0, 30 or 60; the game's own frame step", e); t = 0; }
    float tf = (float)t, sc = 1.0f;
    memcpy(dm_fps_state, &tf, 4);
    const char *rs = getenv("DM_RENDER_SCALE");
    if (rs && f->render_scale) {
        char *end; float v = strtof(rs, &end);
        if (end == rs || *end || !(v >= 0.5f && v <= 1.0f)) RED("DM_RENDER_SCALE=%s: must be 0.5 to 1.0; the game's own size", rs);
        else { sc = v; LOG("%s: 3D views rendered at %.2f of the game's size (DM_RENDER_SCALE)", m->name, sc); }
    }
    memcpy(dm_fps_state + 0x10, &sc, 4);
    LOG("%s: frame step installed (%u stub words, %u sites): %s", m->name, f->nwords, f->nputs,
        t ? (t == 60 ? "steps decided by the host, up to 60 fps (DM_FPS_TARGET)" : "steps decided by the host, capped at 30 fps (DM_FPS_TARGET)")
          : "the game's own frame step (DM_FPS_TARGET=60 for 60 fps)");
}

// Loads an NSO module: its segments decompressed (LZ4) into the guest's space, its build's cuts and frame step applied.
module_t *nso_load(const char *path) {
    if (g_nmodules == NXL_MAX_MODULES) die("too many modules");
    size_t fsize;
    uint8_t *f = read_file(path, &fsize);
    if (fsize < sizeof(nso_header) || memcmp(f, "NSO0", 4)) die("%s: not an NSO file", path);
    const nso_header *h = (const nso_header *)f;
    uint64_t image = (uint64_t)h->seg[2].mem_off + h->seg[2].size + h->seg[2].extra;
    module_t *m = calloc(1, sizeof *m);
    const char *slash = strrchr(path, '/');
    snprintf(m->name, sizeof m->name, "%s", slash ? slash + 1 : path);
    m->size = (image + 0xFFF) & ~0xFFFull;
    const nxl_fstep *fs = fstep_of(h->build_id);
    if (fs) {   // the frame step's patch area and state page, after the image (install_fstep)
        m->fs_off = m->size;
        m->fs_size = (4ull * fs->nwords + 0xFFF) & ~0xFFFull;
        m->fs_state_off = m->fs_off + m->fs_size;
    }
    m->base = (uint8_t *)mem_map_module(m->size + (fs ? m->fs_size + FS_STATE_SIZE : 0));
    for (int i = 0; i < 3; i++) {
        uint8_t *dst = m->base + h->seg[i].mem_off;
        uint32_t size = h->seg[i].size, csize = h->csize[i], off = h->seg[i].file_off;
        if ((uint64_t)h->seg[i].mem_off + size > m->size) die("%s: segment %d outside the image", path, i);
        if (h->flags & (1u << i)) {   // LZ4-compressed segment
            if ((uint64_t)off + csize > fsize) die("%s: segment %d outside the file", path, i);
            int n = LZ4_decompress_safe((const char *)f + off, (char *)dst, (int)csize, (int)size);
            if (n != (int)size) die("%s: segment %d does not decompress (%d of %u bytes)", path, i, n, size);
        } else {
            if ((uint64_t)off + size > fsize) die("%s: segment %d outside the file", path, i);
            memcpy(dst, f + off, size);
        }
    }
    m->text_off = h->seg[0].mem_off, m->text_size = h->seg[0].size;
    m->ro_off = h->seg[1].mem_off, m->ro_size = h->seg[1].size;
    m->data_off = h->seg[2].mem_off, m->data_size = h->seg[2].size, m->bss_size = h->seg[2].extra;
    memcpy(m->build_id, h->build_id, 32);
    free(f);
    int cut = apply_cuts(m);
    if (fs && !cut) {
        const char *t = getenv("DM_FPS_TARGET");
        if (t && atoi(t)) RED("%s: DM_FPS_TARGET=%s is not used: the frame step needs the build's cuts, which NXL_NO_CUTS leaves out", m->name, t);
        else LOG("%s: frame step not installed: the build's cuts are not in (NXL_NO_CUTS)", m->name);
        fs = NULL; m->fs_size = 0;
    }
    if (fs) install_fstep(m, fs);
    { void exefs_patches_apply(module_t *m); exefs_patches_apply(m); }   // mods' code patches (exefs_patch.c)
    parse_dynamic(m);
    g_modules[g_nmodules++] = m;
    LOG("loaded %s at %#llx (%#llx bytes, %u symbols)", m->name, (unsigned long long)(uintptr_t)m->base,
        (unsigned long long)m->size, m->nsyms);
    return m;
}

// A GPU driver of the game's (NVIDIA's OpenGL / EGL driver, the Vulkan loader or driver, subsdk*): a module that is not
// the game's main and defines EGL's or Vulkan's entry points. nxlinux's bridges (gfx/hle_egl.c, gfx/hle_vk.c) take every
// call to them to the PC's driver, so its code is never needed and its constructors are not run (src/main.c)
int loader_is_gpu_driver(module_t *m) {
    return !module_find(m, "nnMain") && (module_find(m, "eglGetDisplay") || module_find(m, "vkCreateInstance") ||
                                          module_find(m, "NvGlEs2Init") || module_find(m, "vk_icdGetInstanceProcAddr"));
}
// what the game draws with, from its modules' imports (a driver's own are not counted): NVN, OpenGL (EGL) or Vulkan
static unsigned g_api_imports[3];
static int api_of(const char *n) {   // 0 NVN, 1 OpenGL / EGL, 2 Vulkan, -1 none
    if (!strncmp(n, "nvn", 3) && n[3] >= 'A' && n[3] <= 'Z') return 0;
    if ((!strncmp(n, "egl", 3) && n[3] >= 'A' && n[3] <= 'Z') || (!strncmp(n, "gl", 2) && n[2] >= 'A' && n[2] <= 'Z')) return 1;
    if (!strncmp(n, "vk", 2) && n[2] >= 'A' && n[2] <= 'Z') return 2;
    return -1;
}
const char *loader_gfx_api(void) {   // "NVN", "OpenGL", "Vulkan" or "none" (the most imports; NVN wins a tie)
    int b = g_api_imports[0] >= g_api_imports[1] && g_api_imports[0] >= g_api_imports[2] ? 0 : g_api_imports[1] >= g_api_imports[2] ? 1 : 2;
    return !g_api_imports[b] ? "none" : b == 0 ? "NVN" : b == 1 ? "OpenGL" : "Vulkan";
}
// The address a symbol resolves to: a module's own, a host stand-in (HLE), a graphics bridge, or a stub that reports
// the missing import.
static uint64_t bind(module_t *m, const Elf64_Sym *s) {
    if (s->st_shndx != SHN_UNDEF && ELF64_ST_BIND(s->st_info) == STB_LOCAL) return (uint64_t)m->base + s->st_value;
    const char *name = m->strtab + s->st_name;
    if (s->st_shndx == SHN_UNDEF) {
        int api = api_of(name);
        static module_t *last; static int last_drv;
        if (api >= 0) {
            if (m != last) last = m, last_drv = loader_is_gpu_driver(m);
            if (!last_drv) g_api_imports[api]++;
        }
    }
    uint64_t a = hle_lookup(name, 1);   // forced host implementations come first
    if (a) {
        if (!strncmp(name, "_ZN5movie7Decoder", 17)) {   // the movie decoder: say which entry points the host takes (hle_movie.c)
            static int n;
            if (n++ < 24) LOG("%s's movie::Decoder import %s: the host's decoder", m->name, name);
        }
        return a;
    }
    for (int i = 0; i < g_nmodules; i++) {
        const Elf64_Sym *d = module_find(g_modules[i], name);
        if (d) {
            uint64_t a2 = (uint64_t)g_modules[i]->base + d->st_value;
            return ELF64_ST_TYPE(d->st_info) == STT_FUNC ? hle_trace_stub(name, a2) : a2;
        }
    }
    if ((a = hle_lookup(name, 0))) return a;
    if (ELF64_ST_BIND(s->st_info) == STB_WEAK) return 0;
    if (ELF64_ST_TYPE(s->st_info) == STT_OBJECT) die("%s: imports the variable %s, which nothing defines", m->name, name);
    g_nmissing++;
    return hle_missing(name);
}

// Applies a module's relocations (RELA, the PLT, RELR) against the symbols of all modules.
static void relocate(module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
    for (const Elf64_Dyn *d = (const Elf64_Dyn *)m->dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
    }
    const Elf64_Sym *st = m->symtab;
    unsigned counts[4] = {0};
    uint64_t tables[2][2] = {{rela, relasz}, {jmprel, pltsz}};
    for (int t = 0; t < 2; t++) {
        const Elf64_Rela *r = (const Elf64_Rela *)(m->base + tables[t][0]);
        for (size_t i = 0; tables[t][0] && i < tables[t][1] / sizeof *r; i++, r++) {
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            uint32_t type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            switch (type) {
            case R_AARCH64_RELATIVE: *where = (uint64_t)m->base + r->r_addend; counts[0]++; break;
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT:
            case R_AARCH64_ABS64: {
                uint64_t v = bind(m, &st[si]);
                *where = v + (type == R_AARCH64_JUMP_SLOT ? 0 : r->r_addend);
                counts[type == R_AARCH64_JUMP_SLOT ? 2 : 1]++;
                break;
            }
            case R_AARCH64_NONE: break;
            default: RED("%s: relocation type %u at %#llx is not supported", m->name, type, (unsigned long long)r->r_offset);
            }
        }
    }
    // DT_RELR (SDK 20 and later): relative relocations as addresses and bitmaps, the addend in the slot
    const uint64_t *e = (const uint64_t *)(m->base + relr);
    uint64_t *where = NULL;
    for (size_t i = 0; relr && i < relrsz / 8; i++) {
        if (!(e[i] & 1)) {
            where = (uint64_t *)(m->base + e[i]);
            *where++ += (uint64_t)m->base;
            counts[3]++;
        } else {
            if (!where) die("%s: DT_RELR bitmap with no address before it", m->name);
            for (uint64_t bits = e[i] >> 1, k = 0; bits; bits >>= 1, k++)
                if (bits & 1) where[k] += (uint64_t)m->base, counts[3]++;
            where += 63;
        }
    }
    LOG("%s: %u relative, %u symbol, %u PLT, %u RELR relocations", m->name, counts[0], counts[1], counts[2], counts[3]);
}

// Links every loaded module, then makes their code and read-only data read-only.
void loader_link_all(void) {
    for (int i = 0; i < g_nmodules; i++) relocate(g_modules[i]);
    for (int i = 0; i < g_nmodules; i++) {   // text and ro are read-only from now on (Horizon maps them so too)
        module_t *m = g_modules[i];
        mem_protect((uint64_t)m->base + m->text_off, (m->ro_off - m->text_off), 0, 3, 5);
        mem_protect((uint64_t)m->base + m->ro_off, (m->data_off - m->ro_off), 0, 3, 1);
        mem_protect((uint64_t)m->base + m->data_off, m->size - m->data_off, 1, 4, 3);
        if (m->fs_size) {   // the frame step's stubs: code; its state page: data the host and the stubs share
            mem_protect((uint64_t)m->base + m->fs_off, m->fs_size, 0, 3, 5);
            mem_protect((uint64_t)m->base + m->fs_state_off, FS_STATE_SIZE, 1, 4, 3);
        }
    }
    if (g_nmissing) RED("%d imports have no implementation: a call to one stops the program with its name", g_nmissing);
    fprintf(stderr, "[nxl] graphics: the game draws with %s%s (imports: %u NVN, %u OpenGL / EGL, %u Vulkan)\n", loader_gfx_api(),
            strcmp(loader_gfx_api(), "NVN") && strcmp(loader_gfx_api(), "none") ? ", on the PC's own driver" : "", g_api_imports[0], g_api_imports[1], g_api_imports[2]);
    for (int i = 0; i < g_nmodules; i++)
        if (loader_is_gpu_driver(g_modules[i])) fprintf(stderr, "[nxl] %s is the game's GPU driver for the Switch: its calls go to the PC's driver instead\n", g_modules[i]->name);
}
int loader_unresolved(void) { return g_nmissing; }

// for compat/nxnative/compat_dm.c, which builds nxnative's module table (it cannot include this file's headers)
typedef struct {
    const char *name; uint8_t *base; uint64_t size, ro_off, data_off;
    const uint8_t *build_id; const void *symtab; const char *strtab; unsigned nsym;
    uint64_t init, init_array, init_arraysz; const void *dynamic;
} nxl_module_info;
int nxl_module_get(int i, nxl_module_info *o);
// Information on loaded module i, for the parts of nxnative that ask; 0 when there is none.
int nxl_module_get(int i, nxl_module_info *o) {
    if (i < 0 || i >= g_nmodules) return 0;
    module_t *m = g_modules[i];
    *o = (nxl_module_info){m->name, m->base, m->size, m->ro_off, m->data_off, m->build_id, m->symtab, m->strtab,
                           m->nsyms, m->init, m->init_array, m->init_arraysz, m->dynamic};
    return 1;
}

// the function symbol at or before addr in its module (for crash reports): NULL when there is none
const char *module_symbolize(uint64_t addr, uint64_t *off, const char **mod) {
    module_t *m = module_of(addr);
    if (!m) return NULL;
    const Elf64_Sym *st = m->symtab, *best = NULL;
    uint64_t rel = addr - (uint64_t)m->base;
    for (uint32_t i = 1; i < m->nsyms; i++) {
        if (st[i].st_shndx == SHN_UNDEF || ELF64_ST_TYPE(st[i].st_info) != STT_FUNC || st[i].st_value > rel) continue;
        if (!best || st[i].st_value > best->st_value) best = &st[i];
    }
    if (mod) *mod = m->name;
    if (!best) { *off = rel; return NULL; }
    *off = rel - best->st_value;
    return m->strtab + best->st_name;
}
