// SPDX-License-Identifier: GPL-3.0-or-later
// The parts of nxnative's host API that use its own types (dm.h's module_t), compiled against nxnative's dm.h:
// the module table its service files read, symbol and SDK-version queries, and the Mac-specific host layers they call
// (graphics, audio output, front end, diagnostics) as Linux stand-ins until those layers are ported.
#include "dm.h"
#include <pthread/qos.h>
#include <stdatomic.h>
#include <strings.h>
#include <time.h>

// ---------- the module table (nxnative's loader.c keeps it; here it mirrors nxlinux's loader) ----------
typedef struct {
    const char *name; uint8_t *base; uint64_t size, ro_off, data_off;
    const uint8_t *build_id; const void *symtab; const char *strtab; unsigned nsym;
    uint64_t init, init_array, init_arraysz; const void *dynamic;
} nxl_module_info;
int nxl_module_get(int i, nxl_module_info *o);
void *host_callable(uint64_t guest_fn);

module_t g_mod[MAX_MOD];
int g_nmod;
uint64_t g_t0_ns;
void nxl_compat_modules_ready(void);
void nxl_compat_modules_ready(void) {   // main.c, once every module is loaded and linked
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    g_t0_ns = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    nxl_module_info mi;
    for (g_nmod = 0; g_nmod < MAX_MOD && nxl_module_get(g_nmod, &mi); g_nmod++) {
        module_t *m = &g_mod[g_nmod];
        memset(m, 0, sizeof *m);
        snprintf(m->name, sizeof m->name, "%s", mi.name);
        char *dot = strstr(m->name, ".nso");
        if (dot) *dot = 0;
        m->base = mi.base;
        m->size = m->image_size = mi.size;
        m->ro_off = mi.ro_off, m->data_off = mi.data_off;
        m->key_page_off = mi.size;   // nxnative's key page follows the image; here the image simply ends
        for (int k = 0; k < 16; k++) snprintf(m->build_id + 2 * k, 3, "%02x", mi.build_id[k]);
        m->dyn = mi.dynamic;
        m->symtab = mi.symtab, m->strtab = mi.strtab, m->nsym = mi.nsym;
        m->init = mi.init, m->init_array = mi.init_array, m->init_arraysz = mi.init_arraysz;
        m->loaded = 1;
    }
}

// The address of a module's exported symbol, or NULL.
void *module_sym(module_t *m, const char *name) {
    for (unsigned k = 1; k < m->nsym; k++) {
        const Elf64_Sym *s = &m->symtab[k];
        if (s->st_shndx && ELF64_ST_BIND(s->st_info) != STB_LOCAL && !strcmp(m->strtab + s->st_name, name)) {
            uint64_t a = (uint64_t)m->base + s->st_value;
            return (s->st_info & 0xF) == 2 ? host_callable(a) : (void *)a;   // STT_FUNC: callable from host code
        }
    }
    return NULL;
}

int dm_main_imports(const char *prefix) {   // nxnative's loader.c
    size_t n = strlen(prefix);
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        module_t *m = &g_mod[i];
        for (unsigned k = 1; k < m->nsym; k++)
            if (!m->symtab[k].st_shndx && !strncmp(m->strtab + m->symtab[k].st_name, prefix, n)) return 1;
    }
    return 0;
}

int dm_sdk_major(void) {   // nxnative's loader.c: the version string in the sdk module ("NintendoSDK_nnSdk-16_2_0")
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7)
            if (p - b >= 11 && !strncasecmp(p - 11, "NintendoSDK", 11) && p[7] >= '0' && p[7] <= '9') {
                major = atoi(p + 7);
                // the SDKs before 1.0 (Breath of the Wild 1.6.0's 0.12.14, the launch year's games) are numbered 0_x:
                // the oldest group (DM_SDK_OLD: NVN API 53, the old shared-font layout), not "unknown"
                if (!major) {
                    major = 1;
                    LOG("sdk: NintendoSDK nnSdk-%.12s (a pre-1.0 SDK, treated as the oldest: major 1)", p + 7);
                    // its NVN is API 52: Breath of the Wild 1.6.0 (nnSdk 0.12.14) creates its graphics only for major 52 and
                    // minor 32 or more (main+0x9fc658); nxnative's oldest group reports 53. DM_NVN_API set by hand wins
                    if (setenv("DM_NVN_API", "52.32", 0) == 0 && !strcmp(getenv("DM_NVN_API"), "52.32"))
                        LOG("sdk: a pre-1.0 SDK's NVN: device API 52.32 (DM_NVN_API overrides)");
                } else LOG("sdk: NintendoSDK nnSdk-%.12s (major %d)", p + 7, major);
                break;
            }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module");
    return major;
}

// NRO modules (nn::ro): not supported yet
module_t *nro_find(const uint8_t sha256[32]) { (void)sha256; return NULL; }
int nro_refused(const uint8_t sha256[32]) { (void)sha256; return 0; }
void nro_load(module_t *m) { die("nn::ro: loading NRO module %s is not supported yet", m->name); }
void nro_unload(module_t *m) { (void)m; }

// ---------- nxnative's thread and clock layer (hle_thread.c, hle_os.c): what its service files call ----------
static _Atomic uint64_t g_guest_frame;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }   // TODO: the guest clock does not freeze yet
void dm_thread_native_wait(int entering) { (void)entering; }   // no guest core accounting yet
void dm_thread_mark_host(void) {}
void dm_host_helper_qos(qos_class_t q) { (void)q; }
// a host lock an HLE call holds while it runs guest code (hle_movie.c): no guest core accounting here, so a plain lock
void dm_host_lock(pthread_mutex_t *m) { pthread_mutex_lock(m); }
// Darwin's pthread_cond_timedwait_relative_np, for a condition variable on the default (real-time) clock
int nxl_cond_wait_rel(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += rel->tv_sec, ts.tv_nsec += rel->tv_nsec;
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    return pthread_cond_timedwait(c, m, &ts);
}
void dm_backtrace(uint64_t fp) { LOG("(backtrace from fp %#llx not available)", (unsigned long long)fp); }

// ---------- graphics, front end and diagnostics: not ported yet ----------
void dm_gfx_shot_at(long k, const char *path) { (void)k; (void)path; }
void dm_gfx_texraw(uint64_t serial, const char *path) { (void)serial; (void)path; }
void dm_gfx_texlist(int target) { (void)target; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_ctl_peek(uint64_t a, int n) { (void)a; (void)n; }
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path) { (void)a; (void)n; (void)path; }
void dm_ctl_cvar_i32(uint64_t off, int32_t v) { (void)off; (void)v; }
void dm_ctl_cvar_f32(uint64_t off, float v) { (void)off; (void)v; }
// the software keyboard's text entry: no on-screen keyboard yet, so the player cancels
int front_text_input(const char *title, const char *sub, const char *guide, const char *initial, int max_chars, char *out, int out_cap) {
    (void)title; (void)sub; (void)guide; (void)max_chars;
    LOG("[RED] software keyboard: no text input on this host yet (cancelled)");
    if (out_cap > 0) snprintf(out, out_cap, "%s", initial ? initial : "");
    return 0;
}

// audio output (dm_pcmout_*, dm_audio_suspend): pcmout.c

// module_of: compiled as dm_compat_module_of (nxlinux's own module_of returns its own module type)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++)
        if ((const uint8_t *)p >= g_mod[i].base && (const uint8_t *)p < g_mod[i].base + g_mod[i].size) return &g_mod[i];
    return NULL;
}

// nxnative's diagnostic switches, read once at start as its main.c does
struct dm_flags g_flag;
__attribute__((constructor)) static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"), g_flag.svc = !!getenv("DM_SVC"), g_flag.fs = !!getenv("DM_FS");
    g_flag.dbg = !!getenv("DM_DBG"), g_flag.imports = !!getenv("DM_IMPORTS"), g_flag.bootprof = !!getenv("DM_BOOTPROF");
}
// page fault hooks and write preparation (nxnative's crash.c): the graphics layer write-protects texture pages to see the
// game's writes (nvn_dirty.c). Faults go to nxlinux's handler (src/cpu.c), which offers them to the hooks first; host
// code about to write guest memory (a file read into a texture, say) prepares the range first
void nxl_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) { nxl_fault_hook_register(fn); }
static void (*g_write_prep[4])(void *addr, size_t len);
static _Atomic int g_nwrite_prep;
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    int i = atomic_fetch_add(&g_nwrite_prep, 1);
    if (i >= 4) die("too many guest write preparation hooks");
    g_write_prep[i] = fn;
}
void dm_guest_write_prepare(void *addr, size_t len) {
    for (int i = 0; i < atomic_load(&g_nwrite_prep) && i < 4; i++) if (g_write_prep[i]) g_write_prep[i](addr, len);
}
