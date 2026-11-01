// SPDX-License-Identifier: GPL-3.0-or-later
// A null backend behind nxnative's NVN layer (dm/gfx/nvn/hle_nvn.c): the interface nxnative's Metal backend
// (nvn_mtl.m, declared in nvn_mtl.h and hle_nvn.c) provides, with no drawing. It keeps everything the game can observe
// without looking at pixels:
//   - submissions replay the recorded commands' CPU-visible effects in order (buffer copies, uniform updates, buffer
//     clears), and apply event and counter writes at the end of the submission, as Metal's completion handlers do;
//     fence syncs take the fence value of the submission that reaches them;
//   - the "GPU" completes each submission when it is committed, so fences and waits are satisfied at once;
//   - there is no window and no display link, so hle_nvn.c paces presents by the clock itself.
// Texture contents are not tracked: copies into and out of textures, and CPU reads of texels, see zeros. The Vulkan
// backend replaces this file.
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "nvn_timestamp.h"

#include "nvn_obj.h"   // nxnative's object and command record layouts (op_hdr, OP_*, sync_t, p_clear_buf)

uint64_t guest_ns(void);
static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---------- submissions ----------
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; } post_op;   // 1 event, 2 counter
static post_op *g_post;
static int g_npost, g_cpost;
static uint64_t g_fence;                    // the latest committed submission (all of them are complete)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;            // held from mtl_sub_begin to mtl_sub_commit
static pthread_mutex_t g_done_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_done_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_stat_draws, g_stat_subs;

static void post(post_op o) {
    if (g_npost == g_cpost) g_post = realloc(g_post, (g_cpost = g_cpost ? 2 * g_cpost : 64) * sizeof *g_post);
    g_post[g_npost++] = o;
}

void mtl_init(void) {}
void mtl_sub_begin(void) { pthread_mutex_lock(&g_mu); g_npost = 0; }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *handles, int n) { (void)handles; (void)n; }

// Runs a command list without drawing anything: only the commands whose effects the game reads (events, counters,
// copies) are done.
static void exec_list(const uint8_t *p, int depth) {
    if (depth > 16) { fprintf(stderr, "[RED] nvn null backend: command lists nested more than 16 deep\n"); return; }
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); exec_list((const uint8_t *)(uintptr_t)t, depth + 1); break; }
        case OP_SIGNAL_EVENT: {
            post_op o = {.kind = 1};
            memcpy(&o.val, a, 8), memcpy(&o.mode, a + 8, 4), memcpy(&o.v, a + 12, 4);
            post(o);
            break;
        }
        case OP_REPORT_COUNTER: {
            post_op o = {.kind = 2};
            uint64_t t;
            memcpy(&t, a, 8), memcpy(&o.addr, a + 8, 8);
            o.type = (uint32_t)t;
            post(o);
            break;
        }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1; break; }
        case OP_COPY_B2B: {   // GPU addresses are CPU addresses (nxnative's memory pools are the game's own memory)
            uint64_t src, dst, n;
            memcpy(&src, a, 8), memcpy(&dst, a + 8, 8), memcpy(&n, a + 16, 8);
            memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
            break;
        }
        case OP_UPDATE_UB: {
            uint64_t dst, n;
            memcpy(&dst, a, 8), memcpy(&n, a + 8, 8);
            memcpy((void *)(uintptr_t)dst, a + 16, n);
            break;
        }
        case OP_CLEAR_BUF: {
            const p_clear_buf *c = (const p_clear_buf *)a;
            uint8_t *d = (uint8_t *)(uintptr_t)c->addr;
            for (uint64_t i = 0; i + 4 <= c->size; i += 4) memcpy(d + i, &c->value, 4);
            break;
        }
        default: g_stat_draws++; break;   // state, draws, dispatches and texture work: nothing to do without a GPU
        }
        if (h.size < 8) { fprintf(stderr, "[RED] nvn null backend: a command record of %u bytes (op %u)\n", h.size, h.op); return; }
        p += h.size;
    }
}
void mtl_exec(const uint8_t *handle) { exec_list(handle, 0); }

// Completes a submission at once: its events and counter writes land, in order.
uint64_t mtl_sub_commit(void) {
    uint64_t now = guest_ns();
    uint64_t ticks = nvn_timestamp_from_ns(now ? now : mono_ns());
    for (int i = 0; i < g_npost; i++) {   // the submission is complete: its event and counter writes land, in order
        post_op *o = &g_post[i];
        if (o->kind == 1) { if (o->mode == 1) *o->val += o->v; else *o->val = o->v; }
        else { uint64_t rec[2] = {o->type == NVN_COUNTER_TYPE_TIMESTAMP || o->type == 0x10 ? ticks : 0, ticks}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec); }
    }
    g_npost = 0;
    g_stat_subs++;
    pthread_mutex_lock(&g_done_mu);
    uint64_t f = __atomic_add_fetch(&g_fence, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&g_done_cv);
    pthread_mutex_unlock(&g_done_mu);
    pthread_mutex_unlock(&g_mu);
    return f;
}
uint64_t mtl_last_fence(void) { return __atomic_load_n(&g_fence, __ATOMIC_ACQUIRE); }
// 1 once the submission with that fence value is complete (here: committed), 0 on timeout
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (fence <= mtl_last_fence()) return 1;
    if (!timeout_ns) return 0;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns;   // NVN's "forever" is a huge timeout
    ts.tv_sec += (time_t)(t / 1000000000ull), ts.tv_nsec += (long)(t % 1000000000ull);
    if (ts.tv_nsec >= 1000000000) ts.tv_sec++, ts.tv_nsec -= 1000000000;
    pthread_mutex_lock(&g_done_mu);
    int r = 0;
    while (!(r = fence <= g_fence))
        if (pthread_cond_timedwait(&g_done_cv, &g_done_mu, &ts)) { r = fence <= g_fence; break; }
    pthread_mutex_unlock(&g_done_mu);
    return r;
}
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait(fence, timeout_ns); }
void mtl_finish(void) {}
void mtl_drain(void) {}

// ---------- presents: no window; hle_nvn.c paces by the clock ----------
#define PRING 64
static double g_pres_s[PRING];
static _Atomic uint64_t g_presents;
// Counts a presented frame (nothing is shown).
uint64_t mtl_present(const tex_t *tex, double period_s, const int crop[4]) {
    (void)tex; (void)period_s; (void)crop;
    uint64_t n = atomic_fetch_add(&g_presents, 1) + 1;
    g_pres_s[n % PRING] = mono_ns() / 1e9;
    if (n == 1) fprintf(stderr, "[nxl] nvn: first frame presented (null backend: nothing is drawn yet)\n");
    return n;
}
uint64_t nxl_frames_presented(void);
uint64_t nxl_frames_presented(void) { return atomic_load(&g_presents); }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { return n && n <= atomic_load(&g_presents) && atomic_load(&g_presents) - n < PRING ? g_pres_s[n % PRING] : -1; }
uint32_t mtl_present_qos(void) { return 0; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_stalled(void) { return 0; }
int mtl_link_rate(void) { return 60; }
// Waits n vsync ticks (60 Hz) or the timeout.
static int sleep_ticks(int n, uint64_t timeout_ns) {
    uint64_t ns = (uint64_t)(n > 0 ? n : 1) * 16666667ull;
    if (timeout_ns && ns > timeout_ns) ns = timeout_ns;
    struct timespec ts = {(time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull)};
    nanosleep(&ts, NULL);
    return 1;
}
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) { return sleep_ticks(vsyncs, timeout_ns); }
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) { return sleep_ticks(ticks, timeout_ns); }
int mtl_fps_target(void) {
    static int t;
    if (!t) { const char *e = getenv("DM_FPS"); t = e && atoi(e) == 30 ? 30 : 60; }
    return t;
}
int mtl_snap_on(void) { return 0; }
uint64_t mtl_snap_acquire_wait(void) { return 0; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_set_frame(uint64_t f) { (void)f; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) { (void)f; (void)gpu_ms; (void)frame_ms; }

// ---------- objects: nothing to create without a GPU ----------
void mtl_set_depth_zero(int on) { (void)on; }
void mtl_set_origin_lower_left(int on) { (void)on; }
// memory pools: kept, so an address can be checked against them (mtl_addr; hle_nvn.c hashes shader code only when its
// pool is registered)
typedef struct { const uint8_t *mem; uint64_t size; } pool_ent;
static pool_ent *g_pools;
static int g_npools, g_cpools;
static pthread_rwlock_t g_pools_mu = PTHREAD_RWLOCK_INITIALIZER;
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_rwlock_wrlock(&g_pools_mu);
    if (g_npools == g_cpools) g_pools = realloc(g_pools, (g_cpools = g_cpools ? 2 * g_cpools : 64) * sizeof *g_pools);
    g_pools[g_npools++] = (pool_ent){mem, size};
    pthread_rwlock_unlock(&g_pools_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_rwlock_wrlock(&g_pools_mu);
    for (int i = 0; i < g_npools; i++) if (g_pools[i].mem == mem) { g_pools[i] = g_pools[--g_npools]; break; }
    pthread_rwlock_unlock(&g_pools_mu);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {   // buf: the pool's memory (no GPU buffer here)
    pthread_rwlock_rdlock(&g_pools_mu);
    int r = 0;
    for (int i = 0; i < g_npools && !r; i++) {
        uint64_t b = (uint64_t)(uintptr_t)g_pools[i].mem;
        if (addr >= b && len <= g_pools[i].size && addr - b <= g_pools[i].size - len) { *buf = (void *)g_pools[i].mem; *off = addr - b; r = 1; }
    }
    pthread_rwlock_unlock(&g_pools_mu);
    return r;
}
void mtl_alias_drop_pool(const void *pool) { (void)pool; }
void mtl_tex_init(const tex_t *t) { (void)t; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_tex_cpu_written(const tex_t *t) { (void)t; }
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_image(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_prog_set(prog_t *p) { (void)p; }   // the Vulkan backend keeps the control sections (patches/hle_nvn.py)
// a CPU read of texels (nvnTextureReadTexels): contents are not tracked, so zeros
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    (void)t; (void)level; (void)layer0;
    size_t h = r->height > 0 ? (size_t)r->height : 1, d = r->depth > 0 ? (size_t)r->depth : 1;
    memset(dst, 0, img ? img * d : row * h * d);
}

// ---------- statistics: none ----------
void mtl_vbuf_stats(uint64_t *a, uint64_t *b, uint64_t *c, uint64_t *d) { *a = *b = *c = *d = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_subs, *draws = g_stat_draws; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = *made = *none = 0; }
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
uint64_t mtl_pkg_miss(void) { return 0; }
uint64_t mtl_pkg_edge_skips(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
void mtl_pso_line(char *buf, size_t n) { snprintf(buf, n, "none (null backend)"); }

// ---------- host pieces the NVN layer calls (nxnative: hle_os.c, front_mac.m, libSystem) ----------
int dm_detclock_enabled(void) { return 0; }
void dm_guest_frame_tick(void) {}
int front_screen_max_fps(void) { return 60; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; (void)buf; (void)nbytes; return -1; }
