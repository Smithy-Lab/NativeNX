// SPDX-License-Identifier: GPL-3.0-or-later
// The replay of nxnative's NVN command records on Vulkan (see vk.h and nvn_obj.h for the record formats).
//
// State records update a copy of the draw state; draws and dispatches resolve it: the program's stages are translated
// (nxl_shader.cpp, cached by program and by what the translation read of the state), a pipeline is made for the fixed
// state (cached by key), the descriptors are written into a set from this submission's pool, and the draw is recorded
// inside a dynamic-rendering pass on the bound render targets. Copies, clears of whole textures, dispatches and barriers
// end the pass; the next draw begins a new one (load and store), with a full memory barrier between them.
//
// The NVN driver's constant segments, which shaders read directly, are built per draw as nxnative's Metal replay does
// (nvn_mtl.m): c0 holds the storage buffer table (base + 0x10 r: address, size), the largest point size at 0x610, the
// draw's base instance at 0x644, and for compute the dispatch size at 0x200 and the uniform buffer table at 0x210;
// c2 holds the bound texture handles (combined at 0x20 + 8 r, images at 0x120 + 8 r, separate textures at 0x168 + 8 r,
// separate samplers at 0x568 + 8 r); c1 is the shader's own constants; c3 + r are the bound uniform buffers.
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/stat.h>
#include "vk.h"
#include <dirent.h>
#include "nxl_shader.h"
#include <math.h>

VkCompareOp vk_compare_op(int f);
VkBufferView vk_tex_buffer_view(vk_tex *x, int storage, int img_format);
void vk_post_event(volatile uint32_t *val, uint32_t mode, uint32_t v);
void vk_post_counter(uint64_t addr, uint32_t type);
uint64_t vk_next_fence(void);
uint64_t vk_stat_subs(void);
void vk_barrier_on(VkCommandBuffer cb);

enum { C2_TEX = 0x20, C2_TEX_SLOTS = 41, C2_IMG = 0x120, C2_IMG_SLOTS = 8, C2_SEPTEX = 0x168, C2_SAMP = 0x568, DRV_SIZE = 0xA00 };
#define NTEX 256

// ---------- the draw state ----------
typedef struct { const tex_t *t; view_t v; uint8_t has_v; } rt_bind;
typedef struct { uint64_t addr, size; } range;
static struct {
    int nrt;
    tex_t rt_t[8], ds_t;
    view_t rt_v[8], ds_v;
    uint8_t rt_has_v[8], rt_on[8], ds_has_v, ds_on;
    float vp[16][6];
    int scissor[4], has_scissor;
    float depth_n, depth_f;
    const prog_t *prog;
    blend_st blend[8];
    color_st color;
    chmask_st chmask;
    ms_st ms;
    poly_st poly;
    ds_st ds;
    uint32_t vattr[16];
    vs_st vstream[16];
    range vbuf[16];
    range ubo[6][18], ssbo[6][16];
    uint64_t tex[6][NTEX], samp[6][128], img[6][8];
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    float poly_factor, poly_units, poly_clamp;
    float blend_color[4];
    int depth_clamp, prim_restart, rast_discard;
    uint32_t prim_restart_idx;
    int64_t copy_row_stride, copy_img_stride;
    uint32_t base_instance, dispatch[3];
} S;
int g_origin_lower_left = 1;
static int g_depth_zero;   // NVN's defaults: GL's lower-left origin, near at -w
void mtl_set_origin_lower_left(int on) { g_origin_lower_left = on; }
void mtl_set_depth_zero(int on) { g_depth_zero = on; }

// The draw state as NVN starts it: no blending, depth range 0..1, no culling, every colour channel written.
static void state_defaults(void) {
    memset(&S, 0, sizeof S);
    S.depth_n = 0, S.depth_f = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){(uint64_t)i, 2, 1, 2, 1, 1, 1, 0, 0, 0};   // ONE / ZERO / ADD
    S.color = (color_st){0, 3, 8};
    S.chmask.mask = 0xFFFFFFFF;
    S.poly = (poly_st){0, 1, 2, 0};   // no culling, CCW front, fill
    S.ds = (ds_st){0, 0, 2, 0, 8, 8, 1, 1, 1, 1, 1, 1};
    for (int f = 0; f < 2; f++) S.st_vmask[f] = S.st_wmask[f] = 0xFF;
}

// ---------- render passes ----------
static int g_pass_on;
static VkExtent2D g_pass_ext;
static VkFormat g_pass_cfmt[8], g_pass_dfmt;
static uint32_t g_pass_ncol, g_pass_samples, g_pass_layers;
static int g_pass_has_stencil;
static int g_pass_scaled;   // the pass's targets are drawn at the resolution scale (vk_tex.c): viewports and scissors scaled
static uint32_t g_rs_tex[4], g_rs_img[2];   // the last write_descriptors: which of the program's textures / images are scaled
static vk_tex *g_pass_ct[8], *g_pass_dt;
static uint64_t g_stat_draws, g_stat_passes;

typedef struct { uint64_t dst, n; const uint8_t *data; int pending; } ub_upd;
static ub_upd *g_upd; static int g_nupd, g_cupd, g_npending;   // this submission's uniform updates (update_ub)
static int g_ub_hold = 1;   // NXL_UB_HOLD=0: every update ends the render pass, as before
static void ub_flush(void);
static int ub_shadow(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out);
// occlusion counting (NVN's SAMPLES_PASSED counter): after the game first resets it, every render pass segment runs
// inside an occlusion query; a report writes the count since the last reset (queries of earlier submissions folded into
// g_occ_accum when they complete)
static int g_occ_on, g_occ_q = -1;
static uint32_t *g_occ_idx, g_occ_n, g_occ_cap;
static uint64_t g_occ_accum;
uint64_t vk_occ_result(uint32_t q);
// Starts counting the samples the following draws pass (an occlusion query), when a counter is open and a pass is on.
static void occ_begin(void) {
    if (!g_occ_on || !g_pass_on || g_occ_q >= 0) return;
    int q = vk_occ_alloc();
    if (q < 0) { VRED_ONCE("more than 8192 occlusion queries in one submission: the rest are not counted"); return; }
    vkCmdBeginQuery(vk_cmd(), g_vk.occ_pool, (uint32_t)q, g_vk.feat.occlusionQueryPrecise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    void vk_occ_mark_scaled(uint32_t q, int on);
    vk_occ_mark_scaled((uint32_t)q, g_pass_scaled);
    g_occ_q = q;
    if (g_occ_n == g_occ_cap) g_occ_idx = realloc(g_occ_idx, (g_occ_cap = g_occ_cap ? 2 * g_occ_cap : 64) * sizeof *g_occ_idx);
    g_occ_idx[g_occ_n++] = (uint32_t)q;
}
static void occ_end(void) {
    if (g_occ_q < 0) return;
    vkCmdEndQuery(vk_cmd(), g_vk.occ_pool, (uint32_t)g_occ_q);
    g_occ_q = -1;
}
void vk_occ_fold(void) {   // the submission completed: its queries' samples into the running count, their indices freed
    for (uint32_t i = 0; i < g_occ_n; i++) g_occ_accum += vk_occ_result(g_occ_idx[i]);
    g_occ_n = 0;
}
// Starts a new submission's occlusion counting (NXL_OCCLUSION=0 turns it off: reports then write 0).
static void occ_reset(void) {
    occ_end();
    static int off = -1;   // NXL_OCCLUSION=0: reports write 0 again (as before occlusion queries were counted)
    if (off < 0) { const char *e = getenv("NXL_OCCLUSION"); off = e && !strcmp(e, "0"); }
    g_occ_n = 0, g_occ_accum = 0, g_occ_on = g_vk.occ_pool != VK_NULL_HANDLE && !off;
    occ_begin();
}
static void occ_report(uint64_t addr) {
    occ_end();
    vk_post_occlusion(addr, g_occ_accum, g_occ_idx, g_occ_n);
    occ_begin();
}

// segments (vk_segment_cut): after NXL_SEGMENT draws (64, as nxnative's sub_cut; 0 = one submission), the next render
// pass end hands the work so far to the GPU; the cut itself happens at the top of the next command, where nothing holds
// the command buffer
static int g_seg_draws, g_cut_due, g_seg_every = -1;
// Ends the render pass in progress, so a copy, clear or compute dispatch can run between draws; notes when to cut the
// submission.
void vk_draw_suspend(void) {
    if (!g_pass_on) return;
    occ_end();
    if (g_seg_every < 0) { const char *e = getenv("NXL_SEGMENT"); g_seg_every = e ? atoi(e) : 64; }
    if (g_seg_every > 0 && g_seg_draws >= g_seg_every) g_cut_due = 1;
    vkCmdEndRendering(vk_cmd());
    vk_barrier();
    g_pass_on = 0;
    ub_flush();
}

// Begins a render pass on the colour and depth targets set now (drawn at the resolution scale when they all can be).
static void pass_begin(void) {
    if (g_pass_on) return;
    VkCommandBuffer cb = vk_cmd();
    VkRenderingAttachmentInfo ca[8], da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, sa = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    uint32_t w = UINT32_MAX, h = UINT32_MAX, layers = UINT32_MAX, nc = 0, samples = 1;
    memset(g_pass_cfmt, 0, sizeof g_pass_cfmt);
    g_pass_dfmt = VK_FORMAT_UNDEFINED, g_pass_has_stencil = 0;
    // The resolution scale: a pass is drawn scaled when every target in it can be (they must keep one size); its
    // targets are made larger now, or back to 1x when one of them cannot follow
    g_pass_scaled = 0;
    if (vk_res_active()) {
        vk_tex *ts[9]; int nt = 0, all = 1;
        for (int i = 0; i < S.nrt && i < 8; i++) if (S.rt_on[i]) ts[nt++] = vk_tex_get(&S.rt_t[i], 0);
        if (S.ds_on) ts[nt++] = vk_tex_get(&S.ds_t, 0);
        for (int i = 0; i < nt; i++) all &= vk_tex_res_eligible(ts[i]) || vk_tex_scaled(ts[i]);
        for (int i = 0; i < nt; i++) if (ts[i] && !vk_tex_rescale(ts[i], all)) all = 0;
        if (!all) for (int i = 0; i < nt; i++) vk_tex_rescale(ts[i], 0);
        g_pass_scaled = all && nt > 0;
    }
    for (int i = 0; i < S.nrt && i < 8; i++) {
        ca[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        g_pass_ct[i] = NULL;
        if (!S.rt_on[i]) continue;
        vk_tex *x = vk_tex_get(&S.rt_t[i], 0);
        VkImageView v = vk_tex_view(x, &S.rt_v[i], S.rt_has_v[i], 0, 1, 0);
        if (!v) continue;
        int dup = 0;   // the same view at an earlier slot (G-buffers attach a texture twice, writing one with mask 0): left out
        for (int j = 0; j < i; j++) dup |= ca[j].imageView == v;
        if (dup && !((S.chmask.mask >> (4 * i)) & 0xF)) continue;   // written through the first slot only: left out
        vk_tex_mark_gpu(x);
        g_pass_ct[i] = x;
        unsigned lv = S.rt_has_v[i] && S.rt_v[i].set_levels ? S.rt_v[i].base_level : 0;
        uint32_t tw, th, td;
        vk_tex_hextent(x, lv, &tw, &th, &td);
        if (tw < w) w = tw;
        if (th < h) h = th;
        uint32_t ly = vk_tex_view_layers();   // what the view holds: the layers the target's view names
        if (ly < layers) layers = ly;
        const vk_fmt *f = vk_format(S.rt_has_v[i] && S.rt_v[i].set_format ? S.rt_v[i].format : S.rt_t[i].d.format);
        g_pass_cfmt[i] = f && !f->decode ? f->vk : VK_FORMAT_R8G8B8A8_UNORM;
        samples = S.rt_t[i].d.samples > 1 ? (uint32_t)S.rt_t[i].d.samples : 1;
        ca[i].imageView = v, ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL, ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        nc = (uint32_t)i + 1;
    }
    g_pass_dt = NULL;
    if (S.ds_on) {
        vk_tex *x = vk_tex_get(&S.ds_t, 0);
        VkImageView v = vk_tex_view(x, &S.ds_v, S.ds_has_v, 0, 1, 0);
        if (v) {
            vk_tex_mark_gpu(x);
            g_pass_dt = x;
            unsigned lv = S.ds_has_v && S.ds_v.set_levels ? S.ds_v.base_level : 0;
            uint32_t tw, th, td;
            vk_tex_hextent(x, lv, &tw, &th, &td);
            if (tw < w) w = tw;
            if (th < h) h = th;
            uint32_t ly = vk_tex_view_layers();
            if (ly < layers) layers = ly;
            const vk_fmt *f = vk_format(S.ds_t.d.format);
            g_pass_dfmt = f ? f->vk : VK_FORMAT_D32_SFLOAT;
            if (g_pass_dfmt == VK_FORMAT_D24_UNORM_S8_UINT && !g_vk.has_d24s8) g_pass_dfmt = VK_FORMAT_D32_SFLOAT_S8_UINT;
            if (g_pass_dfmt == VK_FORMAT_X8_D24_UNORM_PACK32 && 0) g_pass_dfmt = VK_FORMAT_D32_SFLOAT;
            g_pass_has_stencil = f && f->stencil;
            samples = S.ds_t.d.samples > 1 ? (uint32_t)S.ds_t.d.samples : samples;
            da.imageView = v, da.imageLayout = VK_IMAGE_LAYOUT_GENERAL, da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            if (g_pass_has_stencil) sa = da;
            if (f && f->depth == 0) da.imageView = VK_NULL_HANDLE;   // stencil-only
        }
    }
    if (w == UINT32_MAX) w = h = 1, layers = 1;
    if (layers == UINT32_MAX || !layers) layers = 1;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, layers, 0, nc, ca,
                          g_pass_dt && da.imageView ? &da : NULL, g_pass_dt && g_pass_has_stencil ? &sa : NULL};
    vkCmdBeginRendering(cb, &ri);
    g_pass_on = 1, g_pass_ext = (VkExtent2D){w, h}, g_pass_ncol = nc, g_pass_samples = samples, g_pass_layers = layers;
    occ_begin();
    g_stat_passes++;
    VLOG(2, "pass %ux%u colors %u depth %d", w, h, nc, g_pass_dt != NULL);
}

// ---------- shader translation cache ----------
typedef struct shader_var {
    struct shader_var *next;
    uint64_t prog_key;           // the program's code and control sections, hashed
    nxl_sh_state st;
    nxl_sh_result r;
    int failed;
    VkShaderModule mod[NXL_SH_NSTAGE];
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline compute;          // for a compute program
    VkDescriptorSet last_set;    // the last set written, its submission and the hash of what it holds (reuse)
    uint64_t last_sub, last_hash;
} shader_var;
#define SH_BUCKETS 4096
static shader_var *g_sh[SH_BUCKETS];
static uint64_t g_stat_translations, g_stat_translate_fail;

// the control sections, copied at nvnProgramSetShaders (hle_nvn.c lets the game free them after the call), by serial
typedef struct prog_copy { struct prog_copy *next; uint64_t serial; uint8_t ctl[6][0x880]; int n; uint64_t key; } prog_copy;
#define PROG_BUCKETS 8192
static prog_copy *g_progs[PROG_BUCKETS];
static pthread_mutex_t g_prog_mu = PTHREAD_MUTEX_INITIALIZER;

static uint64_t fnv(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
// a word at a time, for the per-draw keys (pipeline keys, descriptor contents): fnv went byte by byte
static uint64_t hash64(const void *p, size_t n, uint64_t h) {
    const uint8_t *b = p;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, b + i, 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
// The shader stage of a program's code, from its header (compute code has none).
static int stage_of_code(const uint8_t *code) {
    uint32_t magic, w0;
    memcpy(&magic, code, 4);
    if (magic == 0x12345679) return NXL_SH_CS;
    if (magic != 0x12345678) return NXL_SH_CS;   // SDK 10's compute code has no header
    memcpy(&w0, code + 0x30, 4);
    switch (w0 >> 10 & 0xF) {
    case 1: return NXL_SH_VS; case 2: return NXL_SH_TCS; case 3: return NXL_SH_TES; case 4: return NXL_SH_GS; case 5: return NXL_SH_FS;
    default: return NXL_SH_VS;
    }
}
// called by hle_nvn.c's nvnProgramSetShaders (patches/hle_nvn.py)
void mtl_prog_set(prog_t *p) {
    prog_copy *c = calloc(1, sizeof *c);
    c->serial = p->serial, c->n = (int)p->nstage;
    for (int i = 0; i < c->n && i < 6; i++)
        if (p->sd[i].control) memcpy(c->ctl[i], p->sd[i].control, 0x880);
    pthread_mutex_lock(&g_prog_mu);
    uint32_t b = (uint32_t)(c->serial % PROG_BUCKETS);
    c->next = g_progs[b], g_progs[b] = c;
    pthread_mutex_unlock(&g_prog_mu);
}
static const prog_t *g_pf_p; static uint64_t g_pf_serial; static prog_copy *g_pf_pc;   // the last lookup
// The copy of a program's shaders kept here, by the program's serial (the last one found is remembered).
static prog_copy *prog_find(const prog_t *p) {
    if (p == g_pf_p && p->serial == g_pf_serial && g_pf_pc) return g_pf_pc;
    pthread_mutex_lock(&g_prog_mu);
    prog_copy *r = g_progs[p->serial % PROG_BUCKETS];
    while (r && r->serial != p->serial) r = r->next;
    if (r) g_pf_p = p, g_pf_serial = p->serial, g_pf_pc = r;
    pthread_mutex_unlock(&g_prog_mu);
    return r;
}
void mtl_prog_release(prog_t *p) {
    pthread_mutex_lock(&g_prog_mu);
    for (prog_copy **pp = &g_progs[p->serial % PROG_BUCKETS]; *pp; pp = &(*pp)->next)
        if ((*pp)->serial == p->serial) { prog_copy *c = *pp; *pp = c->next; if (c == g_pf_pc) g_pf_pc = NULL, g_pf_p = NULL; free(c); break; }
    pthread_mutex_unlock(&g_prog_mu);
}

// The draw state is kept by NVN's stage numbers (NVNshaderStage: vertex 0, fragment 1, geometry 2, tessellation control
// 3, tessellation evaluation 4, compute 5), the translation by the code areas' order (NXL_SH_*)
static int nvn_stage(int sh) {
    static const int m[NXL_SH_NSTAGE] = {0, 3, 4, 2, 1, 5};
    return sh >= 0 && sh < NXL_SH_NSTAGE ? m[sh] : 0;
}
// the c2 word a stage (NXL_SH_*) has at byte offset off (the bound handles), as the driver would have written it
static uint32_t c2_word(int sh, uint32_t off) {
    if (sh < 0 || sh > 5) return 0;
    int stage = nvn_stage(sh);
    if (off >= C2_TEX && off < C2_TEX + 8 * C2_TEX_SLOTS && (off - C2_TEX) % 8 == 0) {
        unsigned r = (off - C2_TEX) / 8;
        if (r >= 32 && r - 32 < C2_IMG_SLOTS && S.img[stage][r - 32]) return (uint32_t)S.img[stage][r - 32];
        return (uint32_t)S.tex[stage][r];
    }
    if (off >= C2_IMG && off < C2_IMG + 8 * C2_IMG_SLOTS && (off - C2_IMG) % 8 == 0) return (uint32_t)S.img[stage][(off - C2_IMG) / 8];
    if (off >= C2_SEPTEX && off < C2_SAMP && (off - C2_SEPTEX) % 8 == 0) return (uint32_t)S.tex[stage][NVN_SEPTEX_SLOT0 + (off - C2_SEPTEX) / 8];
    if (off >= C2_SAMP && off < C2_SAMP + 8 * 128 && (off - C2_SAMP) % 8 == 0) return (uint32_t)S.samp[stage][(off - C2_SAMP) / 8];
    return 0;
}
static uint32_t c0_ssbo_base(int stage) { return stage == NXL_SH_VS ? 0x110 : stage == NXL_SH_FS ? 0x510 : stage == NXL_SH_GS ? 0x410 : stage == NXL_SH_CS ? 0x310 : 0x110; }
// A word of a stage's driver constants (bank 0): the storage buffers' addresses and sizes the shader reads there.
static uint32_t c0_word(int sh, uint32_t off) {
    uint32_t base = c0_ssbo_base(sh);
    int stage = nvn_stage(sh);
    if (off >= base && off < base + 16 * 16) {
        unsigned r = (off - base) / 16, k = (off - base) % 16;
        if (k == 0) return (uint32_t)S.ssbo[stage][r].addr;
        if (k == 4) return (uint32_t)(S.ssbo[stage][r].addr >> 32);
        if (k == 8) return (uint32_t)S.ssbo[stage][r].size;
    }
    return 0;
}

// The translation's view of the state: c2 reads give the slot a handle comes from (offset / 8 + 1, texture part only),
// so a translation depends on the types of the textures in its slots, not on which textures they are
static uint32_t tr_cbuf(void *ud, int stage, uint32_t index, uint32_t off) {
    (void)ud;
    if (index == 2) return off < C2_SAMP ? off / 8 + 1 : 0;
    if (index == 0) return c0_word(stage, off);
    return 0;
}
static const vk_reg_tex *reg_of_handle(uint32_t h) { uint32_t tid = h & 0xFFFFF; return tid ? vk_reg_tex_at(tid) : NULL; }
// For the shader translator: the kind of texture (1D, 2D, cube ...) and its pixel format bound at a texture handle.
static int tr_tex_type(void *ud, int stage, uint32_t handle, int *is_int, int *is_signed, int *pixfmt) {
    (void)ud;
    uint32_t slot = (handle & 0xFFFFF);
    if (!slot) return NXL_TT_2D;
    uint32_t real = c2_word(stage, (slot - 1) * 8);
    const vk_reg_tex *e = reg_of_handle(real);
    if (!e) return NXL_TT_2D;
    int fmt = e->used == 2 && e->v.set_format ? e->v.format : e->t.d.format;
    const vk_fmt *f = vk_format(fmt);
    *is_int = f && f->integer, *is_signed = f && f->is_signed, *pixfmt = vk_fmt_yuzu_pixel(fmt);
    return vk_tex_type_of(&e->t, &e->v, e->used == 2);
}
static const nxl_sh_env g_tr_env = {tr_cbuf, tr_tex_type, NULL};
// the draw's real view, for resolving descriptors
static uint32_t rt_cbuf(void *ud, int stage, uint32_t index, uint32_t off) {
    (void)ud;
    if (index == 2) return c2_word(stage, off);
    if (index == 0) return c0_word(stage, off);
    return 0;
}
static const nxl_sh_env g_rt_env = {rt_cbuf, NULL, NULL};

// Whether a translated shader still fits the draw: each texture type and format it was translated for is still the one
// bound.
static int reads_hold(const nxl_sh_result *r) {
    for (uint32_t i = 0; i < r->nreads; i++) {
        const nxl_sh_read *d = &r->reads[i];
        if (!d->kind) continue;
        int ii = 0, sg = 0, pf = -1;
        int t = tr_tex_type(NULL, d->stage, d->index, &ii, &sg, &pf);
        uint32_t v = d->kind == 1 ? (uint32_t)t : d->kind == 2 ? (uint32_t)pf : d->kind == 3 ? (uint32_t)ii : (uint32_t)sg;
        if (v != d->value) return 0;
    }
    return 1;
}

// The Vulkan descriptor type of a translated shader's resource kind.
static VkDescriptorType desc_type(int kind) {
    switch (kind) {
    case NXL_DK_UBO: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case NXL_DK_SSBO: return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    case NXL_DK_TEXBUF: return VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
    case NXL_DK_IMGBUF: return VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
    case NXL_DK_TEX: return VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    default: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

// What this GPU's Vulkan can do, as the shader translator asks (made once).
static const nxl_sh_caps *caps(void) {
    static nxl_sh_caps c;
    static int done;
    if (!done) {
        c.spirv_version = 0x00010600;
        c.int8 = g_vk.has_shader_int8, c.int16 = g_vk.feat.shaderInt16, c.int64 = g_vk.feat.shaderInt64;
        c.float16 = g_vk.has_float16, c.float64 = g_vk.feat.shaderFloat64;
        c.storage8 = g_vk.has_storage8, c.storage16 = g_vk.has_storage16;
        c.demote = 1, c.vote = 1, c.viewport_layer = g_vk.has_output_layer, c.multi_viewport = g_vk.feat.multiViewport;
        c.storage_image_ext_formats = g_vk.feat.shaderStorageImageExtendedFormats;
        c.typeless_loads = g_vk.feat.shaderStorageImageReadWithoutFormat;
        c.warp_larger = 1;
        c.min_ssbo_align = (uint32_t)g_vk.props.limits.minStorageBufferOffsetAlignment;
        c.nvidia = g_vk.nvidia;
        nxl_sh_set_caps(&c);
        uint32_t up, shift;
        vk_res_params(&up, &shift);
        if (vk_res_active() && !getenv("NXL_TEST_NO_RESCALING_PASS")) nxl_sh_set_scale(up, shift);   // translations follow the resolution scale (yuzu's rescaling pass)
        done = 1;
    }
    return &c;
}

// A Vulkan shader module from SPIR-V words.
static VkShaderModule make_module(const uint32_t *w, size_t n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, w};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) != VK_SUCCESS) VRED_ONCE("vkCreateShaderModule failed");
    return m;
}

// The descriptor set layout and pipeline layout a translated shader's resources need.
static void layout_for(shader_var *v, int compute) {
    VkDescriptorSetLayoutBinding *b = calloc(v->r.ndescs + 1, sizeof *b);
    for (uint32_t i = 0; i < v->r.ndescs; i++)
        b[i] = (VkDescriptorSetLayoutBinding){v->r.descs[i].binding, desc_type(v->r.descs[i].kind), v->r.descs[i].count,
                                              compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS, NULL};
    VkDescriptorSetLayoutCreateInfo ci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, v->r.ndescs, b};
    VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &ci, NULL, &v->dsl));
    free(b);
    VkPushConstantRange pc = {compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_ALL_GRAPHICS, 0, 64};
    VkPipelineLayoutCreateInfo pi = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &v->dsl, 1, &pc};
    VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &pi, NULL, &v->pl));
}

// the program's stages as the translation wants them
static int prog_sources(const prog_t *p, prog_copy *pc, nxl_sh_src src[NXL_SH_NSTAGE], int *compute) {
    memset(src, 0, sizeof(nxl_sh_src) * NXL_SH_NSTAGE);
    *compute = 0;
    int any = 0;
    for (int i = 0; i < pc->n && i < 6; i++) {
        uint32_t len;
        memcpy(&len, pc->ctl[i] + 0x704, 4);
        const uint8_t *code = (const uint8_t *)(uintptr_t)p->sd[i].data;
        if (!code || !len) continue;
        int st = stage_of_code(code);
        src[st] = (nxl_sh_src){code, len, pc->ctl[i]};
        if (st == NXL_SH_CS) *compute = 1;
        any = 1;
    }
    return any;
}

// A program's shaders translated to SPIR-V for this draw's state, or the translation made before (by the program's hash
// and the state).
static shader_var *translate(const prog_t *p, const nxl_sh_state *st, int *is_compute) {
    prog_copy *pc = prog_find(p);
    if (!pc) { VRED_ONCE("a program bound with no shaders set (first case)"); return NULL; }
    nxl_sh_src src[NXL_SH_NSTAGE];
    int compute;
    if (!prog_sources(p, pc, src, &compute)) return NULL;
    *is_compute = compute;
    if (!pc->key) {   // the program's control sections and code, hashed once (the code stays while the program is set)
        uint64_t h = 0xCBF29CE484222325ull;
        for (int i = 0; i < pc->n && i < 6; i++) h = fnv(pc->ctl[i], 0x880, h);
        for (int s = 0; s < NXL_SH_NSTAGE; s++) if (src[s].code) h = fnv(src[s].code, src[s].code_len, h);
        pc->key = h ? h : 1;
    }
    uint64_t key = pc->key;
    uint32_t bkt = (uint32_t)(key % SH_BUCKETS);
    for (shader_var *v = g_sh[bkt]; v; v = v->next)
        if (v->prog_key == key && !memcmp(&v->st, st, sizeof *st) && (v->failed || reads_hold(&v->r))) return v->failed ? NULL : v;
    shader_var *v = calloc(1, sizeof *v);
    v->prog_key = key, v->st = *st;
    caps();
    g_stat_translations++;
    uint64_t t0 = nxl_perf_now();
    if (nxl_sh_translate(src, compute, st, &g_tr_env, &v->r)) {
        g_stat_translate_fail++;
        VRED("shader translation failed (program %016llx): %s", (unsigned long long)key, v->r.err);
        v->failed = 1;
    } else {
        for (int s = 0; s < NXL_SH_NSTAGE; s++) if (v->r.spirv[s]) v->mod[s] = make_module(v->r.spirv[s], v->r.words[s]);
        {   // NXL_DUMP_SPIRV=<program>[,...] (as NXL_TRACE_FMT prints it): the translated SPIR-V into the log, hex words
            const char *e = getenv("NXL_DUMP_SPIRV");
            char want[20];
            snprintf(want, sizeof want, "%016llx", (unsigned long long)key);
            if (e && strstr(e, want))
                for (int s = 0; s < NXL_SH_NSTAGE; s++) {
                    if (!v->r.spirv[s]) continue;
                    fprintf(stderr, "[vk] spirv %s stage %d %zu words\n", want, s, v->r.words[s]);
                    for (size_t i = 0; i < v->r.words[s]; i += 8) {   // one write per line: other threads' logging cannot split it
                        char line[160];
                        int n = snprintf(line, sizeof line, "[vk] spirv %s %d %05zu:", want, s, i);
                        for (size_t j = i; j < i + 8 && j < v->r.words[s]; j++) n += snprintf(line + n, sizeof line - (size_t)n, " %08x", v->r.spirv[s][j]);
                        fprintf(stderr, "%s\n", line);
                    }
                }
        }
        layout_for(v, compute);
        VLOG(1, "translated program %016llx: stages %#x, %u descriptors", (unsigned long long)key, v->r.stages, v->r.ndescs);
    }
    nxl_perf_add(NXL_PERF_TRANSLATE, t0);
    v->next = g_sh[bkt], g_sh[bkt] = v;
    return v->failed ? NULL : v;
}

// ---------- pipelines ----------
typedef struct {
    shader_var *sh;
    uint32_t prim;
    uint32_t nattr; struct { uint32_t loc, binding, fmt, off; } attr[16];
    uint32_t nbind; struct { uint32_t binding, stride, divisor; } bind[16];
    VkFormat cfmt[8]; VkFormat dfmt; uint32_t ncol, samples;
    uint32_t blend_en, logic; blend_st blend[8]; uint32_t chmask;
    uint8_t poly_mode, depth_clamp, a2c, has_stencil;
} pipe_key;
typedef struct pipe_ent { struct pipe_ent *next; pipe_key k; uint64_t h; VkPipeline p; } pipe_ent;
#define PIPE_BUCKETS 4096
static pipe_ent *g_pipes[PIPE_BUCKETS];
static uint64_t g_stat_pipelines;

// The Vulkan topology of an NVN draw primitive.
static VkPrimitiveTopology topo_of(int prim) {
    switch (prim) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 2: case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: case 8: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: case 9: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // triangles, and quads drawn as converted triangle lists
    }
}
// The Vulkan blend factor of an NVN one.
static VkBlendFactor bf_of(int f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO; case 2: return VK_BLEND_FACTOR_ONE; case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR; case 5: return VK_BLEND_FACTOR_SRC_ALPHA; case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA; case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA; case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR; case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE; case 0x10: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 0x11: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR; case 0x12: return VK_BLEND_FACTOR_SRC1_ALPHA; case 0x13: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR; case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
// An advanced blend mode (blend_st.adv, set by nvnBlendStateSetAdvancedMode) replaces the blend functions. The
// Porter-Duff modes with uncorrelated overlap are sums of source and destination weighted by 1, 0, As, Ad and their
// complements (premultiplied colours), which fixed-function factors give exactly (as nxnative's adv_blend); a source that
// is not premultiplied is multiplied by As first, which only a source factor of ONE can take. Other modes keep the
// state's own functions, with a red line
static blend_st adv_blend(blend_st b) {
    if (!b.adv) return b;
    enum { Z = 1, O = 2, SA = 5, ISA = 6, DA = 7, IDA = 8 };
    static const uint8_t pd[][4] = {   // {source, destination} for colour, then for alpha; NVN_BLEND_ADVANCED_MODE_*
        [0x5] = {Z, Z, Z, Z}, [0x6] = {O, Z, O, Z}, [0x7] = {Z, O, Z, O}, [0x8] = {O, ISA, O, ISA}, [0x9] = {IDA, O, IDA, O},
        [0xA] = {DA, Z, DA, Z}, [0xB] = {Z, SA, Z, SA}, [0xC] = {IDA, Z, IDA, Z}, [0xD] = {Z, ISA, Z, ISA}, [0xE] = {DA, ISA, Z, O},
        [0xF] = {IDA, SA, O, Z}, [0x10] = {IDA, ISA, IDA, ISA}, [0x11] = {O, O, O, O}, [0x12] = {O, O, O, O}, [0x13] = {O, O, O, O},
    };
    const uint8_t *f = b.adv < sizeof pd / sizeof *pd && pd[b.adv][0] ? pd[b.adv] : NULL;
    if (!f || b.adv_ovl || (b.adv_npm && f[0] != O && f[0] != Z)) {
        static uint64_t seen;
        if (!(seen >> (b.adv & 63) & 1)) { seen |= 1ull << (b.adv & 63); VRED("advanced blend mode %#x (overlap %d) has no fixed-function equivalent: the state's blend functions are used", (unsigned)b.adv, (int)b.adv_ovl); }
        return b;
    }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0], b.dst_rgb = f[1], b.src_a = f[2], b.dst_a = f[3];
    b.eq_rgb = b.eq_a = 1;
    return b;
}
static VkBlendOp bo_of(int e) {
    switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; }
}
// The Vulkan stencil operation of an NVN one.
static VkStencilOp so_of(int o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP; case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP;
    }
}

// The graphics pipeline for a draw's state, made the first time and kept (in a hash table, the last one used checked
// first).
static VkPipeline pipe_get(const pipe_key *k) {
    static pipe_ent *last;   // consecutive draws mostly share a pipeline
    if (last && !memcmp(&last->k, k, sizeof *k)) return last->p;
    uint64_t h = hash64(k, sizeof *k, 0xCBF29CE484222325ull);
    uint32_t bkt = (uint32_t)(h % PIPE_BUCKETS);
    for (pipe_ent *e = g_pipes[bkt]; e; e = e->next) if (e->h == h && !memcmp(&e->k, k, sizeof *k)) { last = e; return e->p; }
    shader_var *sh = k->sh;
    VkPipelineShaderStageCreateInfo stg[5];
    uint32_t ns = 0;
    static const VkShaderStageFlagBits bits[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                  VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    for (int s = 0; s < 5; s++)
        if (sh->mod[s]) stg[ns++] = (VkPipelineShaderStageCreateInfo){VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, bits[s], sh->mod[s], "main", NULL};
    VkVertexInputAttributeDescription va[16];
    VkVertexInputBindingDescription vb[16];
    VkVertexInputBindingDivisorDescriptionEXT vd[16];
    uint32_t nvd = 0;
    for (uint32_t i = 0; i < k->nattr; i++) va[i] = (VkVertexInputAttributeDescription){k->attr[i].loc, k->attr[i].binding, (VkFormat)k->attr[i].fmt, k->attr[i].off};
    for (uint32_t i = 0; i < k->nbind; i++) {
        vb[i] = (VkVertexInputBindingDescription){k->bind[i].binding, k->bind[i].stride, k->bind[i].divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        if (k->bind[i].divisor > 1 && g_vk.has_vertex_div) vd[nvd++] = (VkVertexInputBindingDivisorDescriptionEXT){k->bind[i].binding, k->bind[i].divisor};
    }
    VkPipelineVertexInputDivisorStateCreateInfoEXT vds = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_EXT, NULL, nvd, vd};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, nvd ? &vds : NULL, 0, k->nbind, vb, k->nattr, va};
    VkPrimitiveTopology topo = topo_of((int)k->prim);
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo, VK_FALSE};
    VkPipelineTessellationStateCreateInfo ts = {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, 3};
    uint32_t nvp = g_vk.feat.multiViewport ? 16 : 1;
    if (nvp > g_vk.props.limits.maxViewports) nvp = g_vk.props.limits.maxViewports;
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, nvp, NULL, nvp, NULL};
    VkPipelineRasterizationDepthClipStateCreateInfoEXT dc = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT, NULL, 0, VK_TRUE};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0,
                                                 k->depth_clamp && g_vk.feat.depthClamp, VK_FALSE,
                                                 k->poly_mode == 0 && g_vk.feat.fillModeNonSolid ? VK_POLYGON_MODE_POINT : k->poly_mode == 1 && g_vk.feat.fillModeNonSolid ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL,
                                                 VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, (VkSampleCountFlagBits)(k->samples ? k->samples : 1), VK_FALSE, 0, NULL, k->a2c, VK_FALSE};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cba[8];
    for (uint32_t i = 0; i < k->ncol; i++) {
        const blend_st bb = adv_blend(k->blend[i]), *b = &bb;
        cba[i] = (VkPipelineColorBlendAttachmentState){(k->blend_en >> i) & 1, bf_of((int)b->src_rgb), bf_of((int)b->dst_rgb), bo_of((int)b->eq_rgb),
                                                       bf_of((int)b->src_a), bf_of((int)b->dst_a), bo_of((int)b->eq_a), (k->chmask >> (4 * i)) & 0xF};
        if (k->cfmt[i] == VK_FORMAT_UNDEFINED) cba[i].blendEnable = VK_FALSE;
        else {   // integer targets cannot blend
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(g_vk.phys, k->cfmt[i], &fp);
            if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT)) cba[i].blendEnable = VK_FALSE;
        }
    }
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, k->logic != 3 && g_vk.feat.logicOp, (VkLogicOp)k->logic, k->ncol, cba};
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
                            VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, sizeof dyn / sizeof *dyn, dyn};
    VkPipelineRenderingCreateInfo rc = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, k->ncol, k->cfmt,
                                        k->dfmt != VK_FORMAT_S8_UINT ? k->dfmt : VK_FORMAT_UNDEFINED, k->has_stencil ? k->dfmt : VK_FORMAT_UNDEFINED};
    if (k->depth_clamp && g_vk.has_depth_clip) rs.pNext = &dc, dc.depthClipEnable = VK_FALSE;
    VkGraphicsPipelineCreateInfo pi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rc, 0, ns, stg, &vi, &ia, sh->mod[NXL_SH_TCS] ? &ts : NULL, &vp, &rs, &ms, &ds, &cb, &dy,
                                       sh->pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p = VK_NULL_HANDLE;
    uint64_t t0 = nxl_perf_now();
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, g_vk.pcache, 1, &pi, NULL, &p);
    nxl_perf_add(NXL_PERF_PIPELINE, t0);
    if (r != VK_SUCCESS) VRED("vkCreateGraphicsPipelines failed (%d)", r);
    pipe_ent *e = calloc(1, sizeof *e);
    e->k = *k, e->h = h, e->p = p, e->next = g_pipes[bkt], g_pipes[bkt] = e;
    last = e;
    g_stat_pipelines++;
    return p;
}
// The compute pipeline of a translated compute shader, made the first time.
static VkPipeline compute_pipe(shader_var *sh) {
    if (sh->compute) return sh->compute;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sh->mod[NXL_SH_CS], "main", NULL}, sh->pl};
    uint64_t t0 = nxl_perf_now();
    if (vkCreateComputePipelines(g_vk.dev, g_vk.pcache, 1, &ci, NULL, &sh->compute) != VK_SUCCESS) VRED("vkCreateComputePipelines failed");
    nxl_perf_add(NXL_PERF_PIPELINE, t0);
    return sh->compute;
}

// ---------- dummy resources for empty slots ----------
static VkImageView g_dummy_view[8];   // by view type
static VkImage g_dummy_img[2];
static VkBufferView g_dummy_bview;
static VkBuffer g_dummy_buf;
// Makes the stand-in images (all zero) bound where a shader samples a texture the game has not set.
static void dummy_init(void) {
    // a 1x1 2D image with 6 layers (2D, 2D array, cube views) and a 1x1x1 3D image, zero-filled
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 6,
                            VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    for (int k = 0; k < 2; k++) {
        if (k == 1) ci.flags = 0, ci.imageType = VK_IMAGE_TYPE_3D, ci.arrayLayers = 1;
        VK_CHECK(vkCreateImage(g_vk.dev, &ci, NULL, &g_dummy_img[k]));
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_vk.dev, g_dummy_img[k], &mr);
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, 0)};
        VkDeviceMemory m;
        VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &m));
        VK_CHECK(vkBindImageMemory(g_vk.dev, g_dummy_img[k], m, 0));
    }
    VkCommandBuffer cb = vk_once_begin();
    for (int k = 0; k < 2; k++) {
        VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                   g_dummy_img[k], {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, k ? 1 : 6}};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
        vkCmdPipelineBarrier2(cb, &di);
        VkClearColorValue cc = {{0}};
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, k ? 1 : 6};
        vkCmdClearColorImage(cb, g_dummy_img[k], VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
    }
    vk_once_end(cb);
    static const VkImageViewType vts[8] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                           VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_2D};
    for (int i = 0; i < 7; i++) {
        if (i <= 1) { g_dummy_view[i] = VK_NULL_HANDLE; continue; }   // 1D views of a 2D image are not allowed; made below
        uint32_t layers = vts[i] == VK_IMAGE_VIEW_TYPE_CUBE || vts[i] == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY ? 6 : vts[i] == VK_IMAGE_VIEW_TYPE_2D_ARRAY ? 1 : 1;
        if (vts[i] == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !g_vk.feat.imageCubeArray) continue;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, g_dummy_img[i == 4], vts[i], VK_FORMAT_R8G8B8A8_UNORM, {0},
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_dummy_view[i]));
    }
    // 1D dummies: a 1D image of their own
    VkImageCreateInfo c1 = ci;
    c1.imageType = VK_IMAGE_TYPE_1D, c1.flags = 0, c1.arrayLayers = 1;
    VkImage i1;
    VK_CHECK(vkCreateImage(g_vk.dev, &c1, NULL, &i1));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, i1, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, 0)};
    VkDeviceMemory m;
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &m));
    VK_CHECK(vkBindImageMemory(g_vk.dev, i1, m, 0));
    cb = vk_once_begin();
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                               i1, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
    vk_once_end(cb);
    for (int i = 0; i < 2; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, i1, vts[i], VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &g_dummy_view[i]));
    }
    // a small buffer for empty buffer slots
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 65536,
                             VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &g_dummy_buf));
    vkGetBufferMemoryRequirements(g_vk.dev, g_dummy_buf, &mr);
    ai = (VkMemoryAllocateInfo){VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &m));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, g_dummy_buf, m, 0));
    void *p;
    VK_CHECK(vkMapMemory(g_vk.dev, m, 0, VK_WHOLE_SIZE, 0, &p));
    memset(p, 0, 65536);
    VkBufferViewCreateInfo bv = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, g_dummy_buf, VK_FORMAT_R8G8B8A8_UNORM, 0, 65536};
    VK_CHECK(vkCreateBufferView(g_vk.dev, &bv, NULL, &g_dummy_bview));
}
// The stand-in image view for a texture type (1D, 2D, 3D, cube ...).
static VkImageView dummy_view(int tt) {
    switch (tt) {
    case NXL_TT_1D: return g_dummy_view[0]; case NXL_TT_1D_ARRAY: return g_dummy_view[1]; case NXL_TT_2D_ARRAY: return g_dummy_view[3];
    case NXL_TT_3D: return g_dummy_view[4]; case NXL_TT_CUBE: return g_dummy_view[5]; case NXL_TT_CUBE_ARRAY: return g_dummy_view[6] ? g_dummy_view[6] : g_dummy_view[5];
    default: return g_dummy_view[2];
    }
}
void vk_draw_init(void) { dummy_init(); state_defaults(); }

// ---------- descriptors ----------
// a buffer range for a descriptor: the pool buffer when the address is in one and aligned as the device needs, else a copy
// A guest buffer range as a descriptor: in its memory pool where it is one (a shader's writes land in guest memory), the
// size cut to what the pool holds after it (the game often gives a buffer's whole size from an offset into it: the
// lookup of the full range failed and the binding fell back to a scratch copy, and a storage buffer's writes were lost),
// the start moved down to the device's alignment (storage buffer offsets in the translated shaders count from there)
int vk_buf_room(uint64_t addr, VkBuffer *buf, VkDeviceSize *off, uint64_t *room);
// A guest buffer range as a descriptor: in its memory pool when it is in one, else copied to scratch memory.
static void buf_range(uint64_t addr, uint64_t size, VkDeviceSize align, uint64_t maxr, VkDescriptorBufferInfo *out, int storage) {
    if (!size) size = 16;
    if (size > maxr) size = maxr;
    VkBuffer b; VkDeviceSize o; uint64_t room;
    if (addr && vk_buf_room(addr, &b, &o, &room)) {
        if (size > room) size = room;
        VkDeviceSize r = o % align;
        uint64_t n = size + r > maxr ? maxr : size + r;
        *out = (VkDescriptorBufferInfo){b, o - r, n};
        if (r && !storage) VRED_ONCE("a uniform buffer at %#llx is not %llu-byte aligned: read from %llu bytes before it", (unsigned long long)addr, (unsigned long long)align, (unsigned long long)r);
        return;
    }
    if (addr && size <= 65536) {   // not in a pool: copied (a storage buffer's writes are lost)
        if (storage) VRED_ONCE("a storage buffer at %#llx is in no memory pool: bound as a copy, its writes are lost", (unsigned long long)addr);
        VkBuffer sb; VkDeviceSize so;
        void *d = vk_scratch(size, align, &sb, &so);
        memcpy(d, (const void *)(uintptr_t)addr, size);
        *out = (VkDescriptorBufferInfo){sb, so, size};
        return;
    }
    if (addr) VRED_ONCE("a %s buffer at %#llx (%llu bytes) is in no memory pool: bound to an empty buffer", storage ? "storage" : "uniform", (unsigned long long)addr, (unsigned long long)size);
    *out = (VkDescriptorBufferInfo){g_dummy_buf, 0, 65536 < maxr ? 65536 : maxr};
}

// c0 / c1 / c2 for a stage, in scratch memory
static VkDescriptorBufferInfo g_c2_info[8], g_c0_info[8];
static uint64_t g_c2_sub[8] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull}, g_c0_sub[8] = {~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull, ~0ull};
static uint8_t g_c2_dirty[8] = {1, 1, 1, 1, 1, 1, 1, 1};
// The descriptor of a stage's own constants (bank 1): the code area after control +0x700, copied for the shader.
static void drv_c1(const prog_copy *pc, int stage, VkDeviceSize al, VkDescriptorBufferInfo *out) {
    VkBuffer sb; VkDeviceSize so;
    for (int i = 0; pc && i < pc->n; i++) {
        const uint8_t *code = (const uint8_t *)(uintptr_t)S.prog->sd[i].data;
        if (!code || stage_of_code(code) != stage) continue;
        uint32_t off, len;
        memcpy(&off, pc->ctl[i] + 0x700, 4), memcpy(&len, pc->ctl[i] + 0x704, 4);
        if (off >= len) break;
        uint32_t n = len - off > 65536 ? 65536 : len - off;
        VkBuffer pb; VkDeviceSize po;
        if (vk_buf_at((uint64_t)(uintptr_t)(code + off), n, &pb, &po) && po % al == 0) { *out = (VkDescriptorBufferInfo){pb, po, n}; return; }
        uint32_t m = (n + 255) & ~255u;
        uint8_t *d = vk_scratch(m, al, &sb, &so);
        memcpy(d, code + off, n);
        memset(d + n, 0, m - n);
        *out = (VkDescriptorBufferInfo){sb, so, m};
        return;
    }
    *out = (VkDescriptorBufferInfo){g_dummy_buf, 0, 65536};
}
// The descriptor of a constant bank a translated shader reads: driver constants, the shader's own, or the game's
// uniform buffers.
static void drv_segment(int stage, int bank, const shader_var *sh, VkDescriptorBufferInfo *out) {
    VkBuffer sb; VkDeviceSize so;
    VkDeviceSize al = g_vk.props.limits.minUniformBufferOffsetAlignment;
    if (bank == 1) {   // the shader's own constants: the code area from control +0x700 (kept per stage for the program)
        const prog_copy *pc = prog_find(S.prog);
        static const prog_copy *c1_pc[8]; static uint64_t c1_sub[8], c1_serial[8]; static VkDescriptorBufferInfo c1_info[8];
        int cs = stage & 7;
        uint64_t sub = vk_stat_subs();
        if (pc && c1_pc[cs] == pc && c1_serial[cs] == pc->serial && c1_sub[cs] == sub) { *out = c1_info[cs]; return; }
        c1_pc[cs] = pc, c1_sub[cs] = sub, c1_serial[cs] = pc ? pc->serial : 0;
        drv_c1(pc, stage, al, out);
        c1_info[cs] = *out;
        return;
    }
    (void)sh;
    // c2 (the texture and sampler handles a stage sees) changes only when the game binds; c0 rarely. Both were built
    // into fresh scratch memory for every draw (about 300 words for c2), which also kept descriptor sets from being
    // reused; now the last one is kept per stage until a bind (c2) or a change (c0)
    int ns0 = nvn_stage(stage) & 7;
    uint64_t sub = vk_stat_subs();
    if (bank == 2 && !g_c2_dirty[ns0] && g_c2_sub[ns0] == sub && g_c2_info[ns0].buffer) { *out = g_c2_info[ns0]; return; }
    static uint8_t c0_last[8][DRV_SIZE], c0_now[DRV_SIZE];
    uint8_t *d = bank == 0 ? c0_now : vk_scratch(DRV_SIZE, al, &sb, &so);
    memset(d, 0, DRV_SIZE);
    if (bank == 0) {
        uint32_t base = c0_ssbo_base(stage);
        int ns = nvn_stage(stage);
        for (int r = 0; r < 16; r++) {
            uint64_t a = S.ssbo[ns][r].addr; uint32_t n = (uint32_t)S.ssbo[ns][r].size;
            memcpy(d + base + 16 * r, &a, 8), memcpy(d + base + 16 * r + 8, &n, 4);
        }
        if (stage != NXL_SH_CS) {
            const float max_point = 2047.0f;
            memcpy(d + 0x610, &max_point, 4);
            memcpy(d + 0x644, &S.base_instance, 4);
        } else {
            memcpy(d + 0x200, S.dispatch, 12);
            for (int r = 0; r < 16; r++) {
                uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size;
                memcpy(d + 0x210 + 16 * r, &a, 8), memcpy(d + 0x210 + 16 * r + 8, &n, 4);
            }
        }
    } else {
        // as c2_word gives them, written directly (about 300 words, for every bind)
        const int st = nvn_stage(stage);
        uint32_t v;
        for (unsigned r = 0; r < C2_TEX_SLOTS; r++) {
            v = (uint32_t)(r >= 32 && r - 32 < C2_IMG_SLOTS && S.img[st][r - 32] ? S.img[st][r - 32] : S.tex[st][r]);
            memcpy(d + C2_TEX + 8 * r, &v, 4);
        }
        for (unsigned r = 0; r < (C2_SAMP - C2_SEPTEX) / 8; r++) { v = (uint32_t)S.tex[st][NVN_SEPTEX_SLOT0 + r]; memcpy(d + C2_SEPTEX + 8 * r, &v, 4); }
        for (unsigned r = 0; r < 128; r++) { v = (uint32_t)S.samp[st][r]; memcpy(d + C2_SAMP + 8 * r, &v, 4); }
    }
    if (bank == 0) {
        int sl = (stage == NXL_SH_CS ? 6 : ns0) & 7;
        if (g_c0_sub[sl] == sub && g_c0_info[sl].buffer && !memcmp(c0_last[sl], c0_now, DRV_SIZE)) { *out = g_c0_info[sl]; return; }
        uint8_t *dd = vk_scratch(DRV_SIZE, al, &sb, &so);
        memcpy(dd, c0_now, DRV_SIZE), memcpy(c0_last[sl], c0_now, DRV_SIZE);
        g_c0_info[sl] = (VkDescriptorBufferInfo){sb, so, DRV_SIZE}, g_c0_sub[sl] = sub;
        *out = g_c0_info[sl];
        return;
    }
    *out = (VkDescriptorBufferInfo){sb, so, DRV_SIZE};
    g_c2_info[ns0] = *out, g_c2_sub[ns0] = sub, g_c2_dirty[ns0] = 0;
}

// The descriptors of a draw. Consecutive draws of a program often bind the same resources: the set is reused then
// (same program, same submission, the same buffers, views and samplers), saving a pool allocation and an update
static VkDescriptorSet write_descriptors(shader_var *sh) {
    if (!sh->r.ndescs) return VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint32_t n = sh->r.ndescs, total = 0;
    for (uint32_t i = 0; i < n; i++) total += sh->r.descs[i].count;
    static VkWriteDescriptorSet *w; static VkDescriptorBufferInfo *bi; static VkDescriptorImageInfo *ii; static VkBufferView *bv;
    static uint32_t cap_w, cap_t;
    if (n > cap_w) cap_w = n * 2, w = realloc(w, cap_w * sizeof *w);
    if (total > cap_t) cap_t = total * 2, bi = realloc(bi, cap_t * sizeof *bi), ii = realloc(ii, cap_t * sizeof *ii), bv = realloc(bv, cap_t * sizeof *bv);
    if (total) memset(bi, 0, total * sizeof *bi), memset(ii, 0, total * sizeof *ii), memset(bv, 0, total * sizeof *bv);
    uint32_t k = 0;
    const uint64_t sub = vk_stat_subs();
    const unsigned gen = atomic_load(&vk_tex_gen);
    memset(g_rs_tex, 0, sizeof g_rs_tex), memset(g_rs_img, 0, sizeof g_rs_img);
    const VkPhysicalDeviceLimits *L = &g_vk.props.limits;
    for (uint32_t i = 0; i < n; i++) {
        const nxl_sh_desc *d = &sh->r.descs[i];
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, VK_NULL_HANDLE, d->binding, 0, d->count, desc_type(d->kind)};
        int st = d->stage, ns = nvn_stage(st);
        switch (d->kind) {
        case NXL_DK_UBO: {
            uint32_t c = d->cbuf_index;
            if (c == 0 || c == 1 || c == 2) drv_segment(st, (int)c, sh, &bi[k]);
            else if (c - 3 < 18 && S.ubo[ns][c - 3].addr) {
                uint64_t ua = S.ubo[ns][c - 3].addr, us = S.ubo[ns][c - 3].size ? S.ubo[ns][c - 3].size : 16;
                if (us > 65536) us = 65536;
                uint64_t want = d->ubo_used && d->ubo_used < us ? (d->ubo_used + 15) & ~15ull : us;   // what the shader reads
                if (!ub_shadow(ua, want, L->minUniformBufferOffsetAlignment, &bi[k])) buf_range(ua, us, L->minUniformBufferOffsetAlignment, 65536, &bi[k], 0);
            }
            else bi[k] = (VkDescriptorBufferInfo){g_dummy_buf, 0, 65536};
            w[i].pBufferInfo = &bi[k++];
            break;
        }
        case NXL_DK_SSBO: {
            uint64_t addr = (uint64_t)rt_cbuf(NULL, st, d->cbuf_index, d->cbuf_offset) | (uint64_t)rt_cbuf(NULL, st, d->cbuf_index, d->cbuf_offset + 4) << 32;
            uint32_t size = rt_cbuf(NULL, st, d->cbuf_index, d->cbuf_offset + 8);
            if (addr) buf_range(addr, size ? size : 16, L->minStorageBufferOffsetAlignment, L->maxStorageBufferRange, &bi[k], 1);
            else bi[k] = (VkDescriptorBufferInfo){g_dummy_buf, 0, 65536};
            w[i].pBufferInfo = &bi[k++];
            break;
        }
        case NXL_DK_TEXBUF: case NXL_DK_IMGBUF: {
            w[i].pTexelBufferView = &bv[k];
            for (uint32_t e = 0; e < d->count; e++) {
                const vk_reg_tex *r = reg_of_handle(nxl_sh_desc_handle(d, e, &g_rt_env));
                VkBufferView v = r ? vk_tex_buffer_view(vk_tex_get(&r->t, 0), d->kind == NXL_DK_IMGBUF, d->img_format) : VK_NULL_HANDLE;
                bv[k++] = v ? v : g_dummy_bview;
            }
            break;
        }
        case NXL_DK_TEX: {
            w[i].pImageInfo = &ii[k];
            for (uint32_t e = 0; e < d->count; e++) {
                uint32_t h = nxl_sh_desc_handle(d, e, &g_rt_env);
                vk_reg_tex *r = (vk_reg_tex *)reg_of_handle(h);
                VkImageView v = VK_NULL_HANDLE;
                if (r && r->t.d.target != 10) {
                    static int cache_on = -1;   // NXL_TEX_CACHE=0: look every texture up for every draw, as before
                    if (cache_on < 0) { const char *ev = getenv("NXL_TEX_CACHE"); cache_on = !(ev && *ev == '0'); }
                    if (cache_on && r->csub == sub && r->cgen == gen) v = r->cview;   // looked up (and uploaded if written) earlier in this submission
                    else {
                        vk_tex *x = vk_tex_get(&r->t, 1);
                        v = vk_tex_view(x, &r->v, r->used == 2, 0, 0, 0), r->cview = v, r->csub = sub, r->cgen = gen;
                        r->cgpu = (uint8_t)vk_tex_gpu_drawn(x), r->cscaled = (uint8_t)vk_tex_scaled(x);
                    }
                    if (v && r->cscaled && d->scale_index < 128) g_rs_tex[d->scale_index / 32] |= 1u << (d->scale_index % 32);
                }
                ii[k++] = (VkDescriptorImageInfo){vk_reg_sampler_for(h >> 20, r && v && r->cgpu), v ? v : dummy_view(d->tex_type), VK_IMAGE_LAYOUT_GENERAL};
            }
            break;
        }
        default: {   // storage images
            w[i].pImageInfo = &ii[k];
            for (uint32_t e = 0; e < d->count; e++) {
                const vk_reg_tex *r = reg_of_handle(nxl_sh_desc_handle(d, e, &g_rt_env));
                VkImageView v = VK_NULL_HANDLE;
                if (r) {
                    vk_tex *x = vk_tex_get(&r->t, 1);
                    // a shader writing it as a storage image cannot follow the scale (yuzu's pass scales reads only): 1x
                    if (d->is_written && vk_res_active()) vk_tex_noscale(x, "a shader writes it as a storage image");
                    v = vk_tex_storage_view(x, &r->v, r->used == 2, d->img_format);
                    if (v && d->is_written) vk_tex_mark_gpu(x);
                    if (v && vk_tex_scaled(x) && d->scale_index < 64) g_rs_img[d->scale_index / 32] |= 1u << (d->scale_index % 32);
                }
                ii[k++] = (VkDescriptorImageInfo){VK_NULL_HANDLE, v ? v : dummy_view(d->tex_type), VK_IMAGE_LAYOUT_GENERAL};
            }
            break;
        }
        }
    }
    uint64_t h = hash64(bi, total * sizeof *bi, 0xCBF29CE484222325ull);
    h = hash64(ii, total * sizeof *ii, h), h = hash64(bv, total * sizeof *bv, h);
    if (sh->last_set && sh->last_sub == sub && sh->last_hash == h) return sh->last_set;
    set = vk_desc_alloc(sh->dsl);
    if (!set) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n; i++) w[i].dstSet = set;
    vkUpdateDescriptorSets(g_vk.dev, n, w, 0, NULL);
    sh->last_set = set, sh->last_sub = sub, sh->last_hash = h;
    return set;
}

// ---------- draws ----------
// A census of one frame's draws by program, when the file ~/nxl_census appears (touch it while the scene is on screen;
// it is removed when the census is printed): per program the draws, vertices x instances, indirect draws, primitive,
// first target, attribute formats and resources. To find which program draws something wrong
// Hiding programs, to see what each draws: NXL_HIDE_PROG=<program>,... skips their draws. The hide cycle (the file
// ~/nxl_hide_cycle, while the scene is on screen) takes a census of a frame, then hides each program that drew into a
// large target in turn, a few frames each, and saves the whole frame at half size as ~/nxl_hide/<n>_<program>.png (first
// one, 000_none, hides nothing): the picture where a wrong thing is gone names the program that draws it
static uint64_t *g_hide_list; static int g_hide_n = -1, g_hide_i = -2, g_hide_wait;
static _Atomic uint64_t g_hide_prog;
static char g_hide_dump[600]; static _Atomic int g_hide_dump_due;
int vk_hide_take_dump(char *path, size_t n) {
    if (!atomic_load(&g_hide_dump_due)) return 0;
    snprintf(path, n, "%s", g_hide_dump);
    atomic_store(&g_hide_dump_due, 0);
    return 1;
}
// Whether draws of a program are skipped (NXL_HIDE_PROG, or the program picked to hide while debugging).
static int hidden(uint64_t prog) {
    static const char *env; static int init;
    if (!init) init = 1, env = getenv("NXL_HIDE_PROG");
    if (prog == atomic_load(&g_hide_prog) && prog) return 1;
    if (!env) return 0;
    char want[20];
    snprintf(want, sizeof want, "%016llx", (unsigned long long)prog);
    return strstr(env, want) != NULL;
}
static void hide_cycle_frame(void) {   // once a frame, during the cycle
    if (g_hide_i < -1 || g_hide_n < 0) return;
    if (atomic_load(&g_hide_dump_due)) return;   // the last picture is not written yet
    if (++g_hide_wait < 4) return;   // a few frames for the scene to settle
    g_hide_wait = 0;
    const char *home = getenv("HOME");
    char dir[512];
    snprintf(dir, sizeof dir, "%s/nxl_hide", home ? home : ".");
    mkdir(dir, 0755);
    uint64_t cur = atomic_load(&g_hide_prog);
    if (g_hide_i == -1) snprintf(g_hide_dump, sizeof g_hide_dump, "%s/000_none.png", dir);
    else snprintf(g_hide_dump, sizeof g_hide_dump, "%s/%03d_%016llx.png", dir, g_hide_i + 1, (unsigned long long)cur);
    atomic_store(&g_hide_dump_due, 1);
    g_hide_i++;
    if (g_hide_i >= g_hide_n) {   // done
        fprintf(stderr, "[vk] hide cycle: done, %d programs\n", g_hide_n);
        atomic_store(&g_hide_prog, 0);
        free(g_hide_list), g_hide_list = NULL, g_hide_n = -1, g_hide_i = -2;
        char p[512];
        snprintf(p, sizeof p, "%s/nxl_hide_cycle", home ? home : ".");
        unlink(p);
        return;
    }
    atomic_store(&g_hide_prog, g_hide_list[g_hide_i]);
}

typedef struct { uint64_t prog; uint32_t draws, indirect, max_inst, prim, rt_fmt, rt_w, rt_h, ds; uint64_t verts; uint32_t attrs[8]; uint8_t nattr, ntex, nssbo, nimg, layer; } census_ent;
static census_ent *g_census; static int g_ncensus, g_census_state; static uint64_t g_census_frame;
// Counts the draws of each program in a frame and prints them when the frame ends (NXL_CENSUS, for debugging).
static void census_draw(shader_var *sh, const pipe_key *k, uint32_t prim, uint32_t count, uint32_t inst, int indirect) {
    uint64_t f = vk_presents();
    if (f != g_census_frame) {
        uint64_t prev = g_census_frame;
        g_census_frame = f;
        if (g_census_state == 2) {   // the frame is over: print
            fprintf(stderr, "[vk] census of frame %llu: %d programs\n", (unsigned long long)prev, g_ncensus);
            for (int i = 0; i < g_ncensus; i++) {
                census_ent *e = &g_census[i];
                fprintf(stderr, "[vk] census %016llx draws %u indirect %u verts %llu max-instances %u prim %u target %#x %ux%u depth %u tex %u ssbo %u img %u attrs",
                        (unsigned long long)e->prog, e->draws, e->indirect, (unsigned long long)e->verts, e->max_inst, e->prim, e->rt_fmt, e->rt_w, e->rt_h, e->ds, e->ntex, e->nssbo, e->nimg);
                for (int a = 0; a < e->nattr; a++) fprintf(stderr, " %u", e->attrs[a]);
                fputc('\n', stderr);
            }
            char p[512]; const char *home = getenv("HOME");
            snprintf(p, sizeof p, "%s/nxl_hide_cycle", home ? home : ".");
            if (!access(p, F_OK) && g_hide_n < 0) {   // start the hide cycle with this census's programs
                g_hide_list = malloc(sizeof *g_hide_list * (size_t)(g_ncensus ? g_ncensus : 1)), g_hide_n = 0;
                for (int i = 0; i < g_ncensus; i++)
                    if (g_census[i].rt_w >= 640 || g_census[i].ds) g_hide_list[g_hide_n++] = g_census[i].prog;
                g_hide_i = -1, g_hide_wait = 0;
                {   // the pictures of an earlier cycle go first, so the folder holds this one's only
                    char hd[512];
                    snprintf(hd, sizeof hd, "%s/nxl_hide", home ? home : ".");
                    DIR *dd = opendir(hd);
                    struct dirent *de;
                    while (dd && (de = readdir(dd))) {
                        size_t l = strlen(de->d_name);
                        if (l > 4 && (!strcmp(de->d_name + l - 4, ".ppm") || !strcmp(de->d_name + l - 4, ".png"))) {
                            char old[1024];
                            snprintf(old, sizeof old, "%s/%s", hd, de->d_name);
                            unlink(old);
                        }
                    }
                    if (dd) closedir(dd);
                }
                fprintf(stderr, "[vk] hide cycle: %d programs, about %d s; pictures in %s/nxl_hide\n", g_hide_n, (g_hide_n + 1) * 5 / 30 + 1, home ? home : ".");
            }
            free(g_census), g_census = NULL, g_ncensus = 0, g_census_state = 0;
            snprintf(p, sizeof p, "%s/nxl_census", home ? home : ".");
            unlink(p);
        } else if (g_census_state == 1) g_census_state = 2;   // collect this frame
        else if (g_hide_n < 0) {   // armed by a file (checked once a frame)
            char p[512], q[512]; const char *home = getenv("HOME");
            snprintf(p, sizeof p, "%s/nxl_census", home ? home : ".");
            snprintf(q, sizeof q, "%s/nxl_hide_cycle", home ? home : ".");
            if (!access(p, F_OK) || !access(q, F_OK)) g_census_state = 1, fprintf(stderr, "[vk] census: armed (frame %llu)\n", (unsigned long long)f);
        }
        hide_cycle_frame();
    }
    if (g_census_state != 2) return;
    int i = 0;
    while (i < g_ncensus && g_census[i].prog != sh->prog_key) i++;
    if (i == g_ncensus) {
        if (g_ncensus % 64 == 0) g_census = realloc(g_census, (size_t)(g_ncensus + 64) * sizeof *g_census);
        census_ent *e = &g_census[g_ncensus++];
        memset(e, 0, sizeof *e);
        e->prog = sh->prog_key, e->prim = prim;
        if (g_pass_ct[0]) { const tex_t *t = vk_tex_desc(g_pass_ct[0]); e->rt_fmt = (uint32_t)t->d.format, e->rt_w = t->d.width, e->rt_h = t->d.height; }
        e->ds = g_pass_dt != NULL;
        for (uint32_t a = 0; a < k->nattr && a < 8; a++) e->attrs[e->nattr++] = k->attr[a].fmt;
        for (uint32_t d = 0; d < sh->r.ndescs; d++) {
            int kd = sh->r.descs[d].kind;
            if (kd == NXL_DK_TEX) e->ntex++; else if (kd == NXL_DK_SSBO) e->nssbo++; else if (kd != NXL_DK_UBO && kd != NXL_DK_TEXBUF) e->nimg++;
        }
    }
    census_ent *e = &g_census[i];
    e->draws++, e->indirect += indirect != 0, e->verts += (uint64_t)count * inst;
    if (inst > e->max_inst) e->max_inst = inst;
}

// NXL_TRACE_FMT: the draws that sample or draw into textures of the traced formats, with their state and constants (the
// first two of each program and target, at most 60): enough to see where a colour goes wrong without the game's code
static void trace_draw(shader_var *sh, const pipe_key *k, int indexed, uint32_t count, uint32_t inst) {
    static int lines;
    static uint64_t seen[128]; static uint8_t times[128]; static int nseen;
    if (lines >= 120) return;
    int hit = 0;
    {   // NXL_TRACE_PROG=<program>,...: those programs' draws too, whatever their textures
        static const char *progs; static int init;
        if (!init) init = 1, progs = getenv("NXL_TRACE_PROG");
        int vk_trace_active(void);
        if (!progs && !vk_trace_active()) return;   // no tracing: nothing to look at
        char want[20];
        snprintf(want, sizeof want, "%016llx", (unsigned long long)sh->prog_key);
        if (progs && strstr(progs, want)) hit = 1;
    }
    for (uint32_t i = 0; i < 8 && !hit; i++) if (g_pass_ct[i]) hit |= vk_trace_wants(g_pass_ct[i], &S.rt_v[i], S.rt_has_v[i]);
    for (uint32_t i = 0; i < sh->r.ndescs && !hit; i++) {
        const nxl_sh_desc *d = &sh->r.descs[i];
        if (d->kind != NXL_DK_TEX) continue;
        const vk_reg_tex *r = reg_of_handle(nxl_sh_desc_handle(d, 0, &g_rt_env));
        if (r && r->t.d.target != 10) hit |= vk_trace_wants(vk_tex_get(&r->t, 0), &r->v, r->used == 2);
    }
    if (!hit) return;
    uint64_t key = sh->prog_key * 31 + (g_pass_ct[0] ? (uint64_t)(uintptr_t)vk_tex_desc(g_pass_ct[0])->mem : 0);
    int j = 0;
    while (j < nseen && seen[j] != key) j++;
    if (j == nseen) { if (nseen == 128) return; seen[nseen++] = key; times[j] = 0; }
    if (times[j]++ >= 2) return;
    lines++;
    fprintf(stderr, "[vk] draw program %016llx %s %u x%u (base instance %u), targets", (unsigned long long)sh->prog_key, indexed ? "indexed" : "arrays", count, inst, S.base_instance);
    for (uint32_t i = 0; i < k->ncol; i++) {
        if (!g_pass_ct[i]) { fprintf(stderr, " -"); continue; }
        const tex_t *t = vk_tex_desc(g_pass_ct[i]);
        const blend_st *b = &k->blend[i];
        fprintf(stderr, " [%u: NVN %#x %ux%u at %p vk %d, blend %s %d %d %d / %d %d %d adv %d, mask %x]", i, t->d.format, t->d.width, t->d.height, (void *)t->mem,
                k->cfmt[i], (k->blend_en >> i & 1) ? "on" : "off", (int)b->src_rgb, (int)b->dst_rgb, (int)b->eq_rgb, (int)b->src_a, (int)b->dst_a, (int)b->eq_a, (int)b->adv,
                (k->chmask >> (4 * i)) & 0xF);
    }
    fprintf(stderr, ", logic %u, alpha func %u, blend colour %g %g %g %g\n", k->logic, (unsigned)S.color.alpha_func, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
    for (uint32_t i = 0; i < sh->r.ndescs; i++) {
        const nxl_sh_desc *d = &sh->r.descs[i];
        if (d->kind == NXL_DK_TEX) {
            uint32_t hd = nxl_sh_desc_handle(d, 0, &g_rt_env);
            const vk_reg_tex *r = reg_of_handle(hd);
            if (!r) { fprintf(stderr, "[vk]     stage %d texture binding %u: none\n", d->stage, d->binding); continue; }
            int sw = r->used == 2 && r->v.set_swizzle;
            const samp_par *sp = vk_reg_samp_at(hd >> 20);
            vk_trace_dump_later(&r->t);
            fprintf(stderr, "[vk]     stage %d texture binding %u: NVN %#x%s%#x %ux%u at %p, mapping (%d %d %d %d), sampler %s\n", d->stage, d->binding, r->t.d.format,
                    r->used == 2 && r->v.set_format ? " view " : " / ", r->used == 2 && r->v.set_format ? r->v.format : r->t.d.format, r->t.d.width, r->t.d.height, (void *)r->t.mem,
                    sw ? r->v.swz[0] : r->t.set_swz ? r->t.swz & 7 : 2, sw ? r->v.swz[1] : r->t.set_swz ? r->t.swz >> 3 & 7 : 3,
                    sw ? r->v.swz[2] : r->t.set_swz ? r->t.swz >> 6 & 7 : 4, sw ? r->v.swz[3] : r->t.set_swz ? r->t.swz >> 9 & 7 : 5, sp ? "set" : "none");
        } else if (d->kind == NXL_DK_UBO && d->cbuf_index >= 3) {
            int ns = nvn_stage(d->stage);
            range u = S.ubo[ns][d->cbuf_index - 3];
            fprintf(stderr, "[vk]     stage %d constants c%u (%llu bytes):", d->stage, d->cbuf_index, (unsigned long long)u.size);
            if (u.addr && u.size <= 1024) {   // all of it, as words (floats and flags alike)
                for (int w = 0; (uint64_t)w * 4 < u.size; w++) fprintf(stderr, "%s%08x", w % 8 ? " " : w ? "\n[vk]       " : " ", ((const uint32_t *)(uintptr_t)u.addr)[w]);
            } else
                for (int w = 0; u.addr && w < 24 && (uint64_t)w * 4 < u.size; w++) fprintf(stderr, " %g", ((const float *)(uintptr_t)u.addr)[w]);
            fputc('\n', stderr);
        } else if (d->kind != NXL_DK_UBO) fprintf(stderr, "[vk]     stage %d resource kind %d binding %u\n", d->stage, d->kind, d->binding);
    }
    for (uint32_t i = 0; i < k->nattr; i++) {
        uint32_t sidx = k->attr[i].binding;
        range vb = S.vbuf[sidx];
        fprintf(stderr, "[vk]     attribute %u: stream %u offset %u vk %u stride %u divisor %u:", k->attr[i].loc, sidx, k->attr[i].off, k->attr[i].fmt, S.vstream[sidx].stride, S.vstream[sidx].divisor);
        const uint8_t *p = (const uint8_t *)(uintptr_t)vb.addr;
        for (int v = 0; p && v < 4 && (uint64_t)(v * S.vstream[sidx].stride + k->attr[i].off + 16) <= vb.size; v++) {
            const uint8_t *q = p + v * S.vstream[sidx].stride + k->attr[i].off;
            uint32_t w[4]; memcpy(w, q, 16);
            fprintf(stderr, " {%08x %08x %08x %08x}", w[0], w[1], w[2], w[3]);
        }
        fputc('\n', stderr);
    }
}


static int frag_type_of(int nvn_fmt) {
    const vk_fmt *f = vk_format(nvn_fmt);
    return f && f->integer ? (f->is_signed ? 1 : 2) : 0;
}
// Whether a vertex attribute format is read as integers (signed / unsigned) or as floats.
static int attr_type_of(int nvn_fmt) {
    switch (nvn_fmt) {   // integer formats read as integers; UI2F / I2F and the rest as floats
    case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: case 0x3E: case 0x6F: case 0x74: case 0x77: return 2;
    case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: case 0x6A: case 0x70: case 0x75: case 0x78: return 1;
    default: return 0;
    }
}

// the dynamic state of a draw; skipped when it is the same as the last draw's in this command buffer (it was set on
// every draw: about 25 calls)
static void set_dynamic(VkCommandBuffer cb) {
    struct {
        VkCommandBuffer cb; unsigned gen; float vp[16][6], dn, df, poly_units, poly_clamp, poly_factor, blend[4];
        int scissor[4], has_scissor, origin, prim_restart, rast_discard, pass_stencil, scaled;
        VkExtent2D ext; VkFormat dfmt; void *dt;
        uint32_t vmask[2], wmask[2], ref[2];
        poly_st poly; ds_st ds;
    } key;
    static __typeof__(key) last;
    memset(&key, 0, sizeof key);
    extern unsigned vk_cb_gen;
    key.cb = cb, key.gen = vk_cb_gen, memcpy(key.vp, S.vp, sizeof key.vp), key.dn = S.depth_n, key.df = S.depth_f;
    key.poly_units = S.poly_units, key.poly_clamp = S.poly_clamp, key.poly_factor = S.poly_factor, memcpy(key.blend, S.blend_color, sizeof key.blend);
    memcpy(key.scissor, S.scissor, sizeof key.scissor), key.has_scissor = S.has_scissor, key.origin = g_origin_lower_left;
    key.prim_restart = S.prim_restart, key.rast_discard = S.rast_discard, key.pass_stencil = g_pass_has_stencil;
    key.ext = g_pass_ext, key.dfmt = g_pass_dfmt, key.dt = g_pass_dt, key.scaled = g_pass_scaled;
    memcpy(key.vmask, S.st_vmask, 8), memcpy(key.wmask, S.st_wmask, 8), memcpy(key.ref, S.st_ref, 8);
    key.poly = S.poly, key.ds = S.ds;
    if (!memcmp(&key, &last, sizeof key)) return;
    last = key;
    VkViewport vp[16];
    uint32_t nvp = g_vk.feat.multiViewport ? 16 : 1;
    if (nvp > g_vk.props.limits.maxViewports) nvp = g_vk.props.limits.maxViewports;
    for (uint32_t i = 0; i < nvp; i++) {
        float x = S.vp[i][0], y = S.vp[i][1], w = S.vp[i][2], h = S.vp[i][3];
        if (g_pass_scaled) { const float f = vk_res_factor(); x *= f, y *= f, w *= f, h *= f; }   // the game's pixels at the scale
        if (w <= 0 || h <= 0) x = 0, y = 0, w = (float)g_pass_ext.width, h = (float)g_pass_ext.height;
        float n = S.depth_n, f = S.depth_f;
        // GL's origin (NVN's default): NDC y = -1 is window row 0, as in Vulkan, so nothing flips (the frame is upside
        // down in memory, and the present flips it); the upper-left origin puts NDC y = +1 at row 0: a flipped viewport
        if (g_origin_lower_left) vp[i] = (VkViewport){x, y, w, h, n, f};
        else vp[i] = (VkViewport){x, y + h, w, -h, n, f};
    }
    vkCmdSetViewport(cb, 0, nvp, vp);
    VkRect2D sc[16];
    for (uint32_t i = 0; i < nvp; i++) {
        if (S.has_scissor) {
            int x = S.scissor[0], y = S.scissor[1], w = S.scissor[2], h = S.scissor[3];
            if (g_pass_scaled) { int x1 = vk_res_up_i(x + w), y1 = vk_res_up_i(y + h); x = vk_res_up_i(x), y = vk_res_up_i(y), w = x1 - x, h = y1 - y; }
            if (x < 0) w += x, x = 0;
            if (y < 0) h += y, y = 0;
            sc[i] = (VkRect2D){{x, y}, {(uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)}};
        } else sc[i] = (VkRect2D){{0, 0}, g_pass_ext};
    }
    vkCmdSetScissor(cb, 0, nvp, sc);
    vkCmdSetLineWidth(cb, 1.0f);
    vkCmdSetDepthBias(cb, S.poly_units, S.poly_clamp, S.poly_factor);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_vmask[0]);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_wmask[0]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, S.st_ref[0]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, S.st_ref[1]);
    VkCullModeFlags cull = S.poly.cull == 1 ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == 2 ? VK_CULL_MODE_BACK_BIT : S.poly.cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
    vkCmdSetCullMode(cb, cull);
    // facing is decided in window rows on both sides (a flipped viewport keeps NVN's rows), so NVN's front face holds
    vkCmdSetFrontFace(cb, S.poly.front == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    int has_depth = g_pass_dt && g_pass_dfmt != VK_FORMAT_S8_UINT;
    vkCmdSetDepthTestEnable(cb, has_depth && S.ds.dt);
    vkCmdSetDepthWriteEnable(cb, has_depth && S.ds.dw);
    vkCmdSetDepthCompareOp(cb, vk_compare_op((int)S.ds.df));
    vkCmdSetStencilTestEnable(cb, g_pass_has_stencil && S.ds.st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, so_of((int)S.ds.fail_f), so_of((int)S.ds.zpass_f), so_of((int)S.ds.zfail_f), vk_compare_op((int)S.ds.sf_f));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, so_of((int)S.ds.fail_b), so_of((int)S.ds.zpass_b), so_of((int)S.ds.zfail_b), vk_compare_op((int)S.ds.sf_b));
    vkCmdSetDepthBiasEnable(cb, S.poly.offset_en != 0);
    vkCmdSetPrimitiveRestartEnable(cb, S.prim_restart);
    vkCmdSetRasterizerDiscardEnable(cb, S.rast_discard);
}

// quads, quad strips, polygons and line loops have no Vulkan topology: indices for a triangle / line list (or a
// rewritten index buffer), written to scratch. Returns the new index count (0: none needed)
static uint32_t convert_prim(int prim, uint32_t first, uint32_t count, const void *src_idx, int itype, VkBuffer *ib, VkDeviceSize *io) {
    if (prim != 7 && prim != 8 && prim != 2) return 0;
    uint32_t n = prim == 7 ? count / 4 * 6 : prim == 8 ? (count >= 4 ? (count - 2) / 2 * 6 : 0) : count + 1;
    uint32_t *o = vk_scratch((VkDeviceSize)n * 4 + 4, 16, ib, io);
    uint32_t k = 0;
    for (uint32_t i = 0; i < count; i++) (void)i;
#define IDX(j) (src_idx ? (itype == 0 ? ((const uint8_t *)src_idx)[j] : itype == 1 ? ((const uint16_t *)src_idx)[j] : ((const uint32_t *)src_idx)[j]) : first + (j))
    if (prim == 7) for (uint32_t q = 0; q + 3 < count; q += 4) { o[k++] = IDX(q); o[k++] = IDX(q + 1); o[k++] = IDX(q + 2); o[k++] = IDX(q); o[k++] = IDX(q + 2); o[k++] = IDX(q + 3); }
    else if (prim == 8) for (uint32_t q = 0; q + 3 < count; q += 2) { o[k++] = IDX(q); o[k++] = IDX(q + 1); o[k++] = IDX(q + 3); o[k++] = IDX(q); o[k++] = IDX(q + 3); o[k++] = IDX(q + 2); }
    else { for (uint32_t j = 0; j < count; j++) o[k++] = IDX(j); if (count) o[k++] = IDX(0); }
#undef IDX
    return k;
}

static void draw_cmds(VkCommandBuffer cb, shader_var *sh, const pipe_key *kp, VkPipeline p, VkDescriptorSet set, int prim, int indexed, const p_draw *d, const p_draw_idx *di);
// Records a draw: the shaders translated, the pipeline chosen, the descriptors and vertex buffers bound, then the draw
// itself.
static void draw_common(int prim, int indexed, const p_draw *d, const p_draw_idx *di) {
    if (!S.prog) return;
    VkCommandBuffer cb = vk_cmd();
    // the translation's draw-state inputs
    nxl_sh_state st;
    memset(&st, 0, sizeof st);
    for (int i = 0; i < 32; i++) st.attr_type[i] = i < 16 ? (uint8_t)attr_type_of(((va_st *)&S.vattr[i])->fmt) : 5;
    for (int i = 0; i < 8; i++) st.frag_type[i] = (uint8_t)(S.rt_on[i] ? frag_type_of(S.rt_has_v[i] && S.rt_v[i].set_format ? S.rt_v[i].format : S.rt_t[i].d.format) : 0);
    st.topology = prim == 0 ? 0 : prim >= 1 && prim <= 3 ? 1 : prim == 10 || prim == 11 ? 2 : prim == 12 || prim == 13 ? 4 : 3;
    st.gl_depth = !g_depth_zero;
    st.points = prim == 0, st.point_size = 1.0f;
    int compute = 0;
    uint64_t t0 = nxl_perf_now();
    shader_var *sh = translate(S.prog, &st, &compute);
    nxl_perf_add(NXL_PERF_D_SHADER, t0);
    if (!sh || compute || !sh->mod[NXL_SH_VS]) return;
    // resources first: texture uploads end an open pass
    S.base_instance = indexed ? (uint32_t)di->base_instance : (uint32_t)d->base_instance;
    uint64_t t1 = nxl_perf_now();
    VkDescriptorSet set = write_descriptors(sh);
    uint64_t t2 = nxl_perf_now();
    atomic_fetch_add_explicit(&nxl_perf_ns[NXL_PERF_D_DESC], t2 - t1, memory_order_relaxed);
    pass_begin();
    pipe_key k;
    memset(&k, 0, sizeof k);
    k.sh = sh, k.prim = (uint32_t)(prim == 7 || prim == 8 ? 4 : prim == 9 ? 6 : prim == 2 ? 1 : prim);
    if (prim == 2) k.prim = 1;
    uint32_t bmask = 0;
    for (int i = 0; i < 16; i++) {
        if (!sh->r.vs_reads_attr[i]) continue;
        va_st a; memcpy(&a, &S.vattr[i], 4);
        VkFormat vf = vk_vertex_format((int)a.fmt);
        if (vf == VK_FORMAT_UNDEFINED) vf = VK_FORMAT_R32G32B32A32_SFLOAT;
        {   // a vertex format the device cannot fetch reads garbage (3-component 8 / 16-bit formats on some GPUs): reported
            static VkFormat checked[64]; static uint8_t ok[64]; static int nchk;
            int j = 0;
            while (j < nchk && checked[j] != vf) j++;
            if (j == nchk && nchk < 64) {
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(g_vk.phys, vf, &fp);
                checked[nchk] = vf, ok[nchk++] = !!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT);
                if (!ok[j]) VRED("vertex format %d (NVN %#x) is not a vertex format on this GPU: those attributes read wrong", vf, (unsigned)a.fmt);
            }
        }
        k.attr[k.nattr++] = (typeof(k.attr[0])){(uint32_t)i, a.stream, (uint32_t)vf, a.offset};
        bmask |= 1u << a.stream;
    }
    for (int s = 0; s < 16; s++)
        if (bmask >> s & 1) k.bind[k.nbind++] = (typeof(k.bind[0])){(uint32_t)s, S.vstream[s].stride, S.vstream[s].divisor};
    memcpy(k.cfmt, g_pass_cfmt, sizeof k.cfmt);
    k.ncol = g_pass_ncol, k.dfmt = g_pass_dt ? g_pass_dfmt : VK_FORMAT_UNDEFINED, k.has_stencil = (uint8_t)g_pass_has_stencil;
    k.samples = g_pass_samples;
    k.blend_en = S.color.blend_en, k.logic = S.color.logic_op, k.chmask = S.chmask.mask;
    for (uint32_t i = 0; i < k.ncol; i++) k.blend[i] = S.blend[i], k.blend[i].target = 0;
    k.poly_mode = (uint8_t)S.poly.mode, k.depth_clamp = (uint8_t)S.depth_clamp, k.a2c = S.ms.a2c;
    census_draw(sh, &k, (uint32_t)prim, indexed ? (uint32_t)di->count : (uint32_t)d->count, indexed ? (uint32_t)(di->instances > 0 ? di->instances : 1) : (uint32_t)(d->instances > 0 ? d->instances : 1), (int)(indexed ? di->ind != 0 : d->ind != 0));
    g_seg_draws++;
    if (hidden(sh->prog_key)) return;
    trace_draw(sh, &k, indexed, indexed ? (uint32_t)di->count : (uint32_t)d->count, indexed ? (uint32_t)(di->instances > 0 ? di->instances : 1) : (uint32_t)(d->instances > 0 ? d->instances : 1));
    VkPipeline p = pipe_get(&k);
    uint64_t t3 = nxl_perf_now();
    atomic_fetch_add_explicit(&nxl_perf_ns[NXL_PERF_D_PIPE], t3 - t2, memory_order_relaxed);
    if (!p) return;
    draw_cmds(cb, sh, &k, p, set, prim, indexed, d, di);
    atomic_fetch_add_explicit(&nxl_perf_ns[NXL_PERF_D_CMD], nxl_perf_now() - t3, memory_order_relaxed);
}
// a draw's commands, once its pipeline and descriptors are there
static void draw_cmds(VkCommandBuffer cb, shader_var *sh, const pipe_key *kp, VkPipeline p, VkDescriptorSet set, int prim, int indexed, const p_draw *d, const p_draw_idx *di) {
    const pipe_key k = *kp;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, sh->pl, 0, 1, &set, 0, NULL);
    if (sh->r.uses_rescaling) {   // which textures / images are scaled, and the fragment position's factor back to the game's pixels
        struct { uint32_t tex[4], img[2]; float down; } rs;
        memcpy(rs.tex, g_rs_tex, sizeof rs.tex), memcpy(rs.img, g_rs_img, sizeof rs.img);
        rs.down = g_pass_scaled ? 1.0f / vk_res_factor() : 1.0f;
        if (sh->r.uses_render_area) VRED_ONCE("a program reads both the render area and the resolution scale (both push constants at 0): the scale wins");
        vkCmdPushConstants(cb, sh->pl, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof rs, &rs);
    } else if (sh->r.uses_render_area) {
        float ra[4] = {0, 0, (float)g_pass_ext.width, (float)g_pass_ext.height};
        if (g_pass_scaled) ra[2] /= vk_res_factor(), ra[3] /= vk_res_factor();
        vkCmdPushConstants(cb, sh->pl, VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16, ra);
    }
    set_dynamic(cb);
    for (uint32_t i = 0; i < k.nbind; i++) {
        uint32_t s = k.bind[i].binding;
        VkBuffer b; VkDeviceSize o;
        uint64_t room;   // in its pool, whatever size the game gave (a size past the pool's end used to bind zeros)
        if (!S.vbuf[s].addr || !vk_buf_room(S.vbuf[s].addr, &b, &o, &room)) {
            if (S.vbuf[s].addr) VRED_ONCE("a vertex buffer at %#llx is in no memory pool: its attributes read 0", (unsigned long long)S.vbuf[s].addr);
            b = g_dummy_buf, o = 0;
        }
        vkCmdBindVertexBuffers(cb, s, 1, &b, &o);
    }
    uint32_t inst = indexed ? (uint32_t)(di->instances > 0 ? di->instances : 1) : (uint32_t)(d->instances > 0 ? d->instances : 1);
    uint64_t ind = indexed ? di->ind : d->ind;
    if ((indexed ? di->mdi_cnt : d->mdi_cnt)) {
        uint32_t cnt = *(const volatile uint32_t *)(uintptr_t)(indexed ? di->mdi_cnt : d->mdi_cnt);
        if ((uint32_t)(indexed ? di->mdi_i : d->mdi_i) >= cnt) return;
        VRED_ONCE("an indirect-count multi-draw: its count is read when the commands are replayed");
    }
    if (ind) {
        VkBuffer b; VkDeviceSize o;
        if (!vk_buf_at(ind, indexed ? 20 : 16, &b, &o)) return;
        if (indexed) {
            VkBuffer ib; VkDeviceSize io;
            if (!vk_buf_at(di->ibuf, 1, &ib, &io)) return;
            vkCmdBindIndexBuffer(cb, ib, io, di->itype == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexedIndirect(cb, b, o, 1, 20);
        } else vkCmdDrawIndirect(cb, b, o, 1, 16);
        g_stat_draws++;
        return;
    }
    if (indexed) {
        uint32_t count = (uint32_t)di->count;
        const void *src = (const void *)(uintptr_t)di->ibuf;
        VkBuffer ib; VkDeviceSize io;
        uint32_t conv = convert_prim(prim, 0, count, src, di->itype, &ib, &io);
        if (conv) {
            vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, conv, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        } else if (di->itype == 0) {   // 8-bit indices: widened
            uint16_t *o = vk_scratch((VkDeviceSize)count * 2 + 4, 16, &ib, &io);
            for (uint32_t i = 0; i < count; i++) o[i] = ((const uint8_t *)src)[i] == 0xFF && S.prim_restart ? 0xFFFF : ((const uint8_t *)src)[i];
            vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        } else {
            uint32_t isz = di->itype == 2 ? 4 : 2;
            if (!vk_buf_at(di->ibuf, (uint64_t)count * isz, &ib, &io)) {
                void *o = vk_scratch((VkDeviceSize)count * isz + 4, 16, &ib, &io);
                memcpy(o, src, (size_t)count * isz);
            }
            vkCmdBindIndexBuffer(cb, ib, io, isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, count, inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        }
    } else {
        VkBuffer ib; VkDeviceSize io;
        uint32_t conv = convert_prim(prim, (uint32_t)d->first, (uint32_t)d->count, NULL, 2, &ib, &io);
        if (conv) {
            vkCmdBindIndexBuffer(cb, ib, io, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, conv, inst, 0, 0, (uint32_t)d->base_instance);
        } else vkCmdDraw(cb, (uint32_t)d->count, inst, (uint32_t)d->first, (uint32_t)d->base_instance);
    }
    g_stat_draws++;
}

// Records a compute dispatch: the shader translated, its descriptors bound, then dispatched outside the render pass.
static void dispatch(const p_dispatch *dp) {
    if (!S.prog) return;
    nxl_sh_state st;
    memset(&st, 0, sizeof st);
    int compute = 0;
    shader_var *sh = translate(S.prog, &st, &compute);
    if (!sh || !compute || !sh->mod[NXL_SH_CS]) return;
    vk_draw_suspend();
    S.dispatch[0] = (uint32_t)dp->x, S.dispatch[1] = (uint32_t)dp->y, S.dispatch[2] = (uint32_t)dp->z;
    if (dp->ind) memcpy(S.dispatch, (const void *)(uintptr_t)dp->ind, 12);   // the counts as the CPU sees them now
    VkDescriptorSet set = write_descriptors(sh);
    vk_draw_suspend();   // descriptors may have uploaded textures
    VkPipeline p = compute_pipe(sh);
    if (!p) return;
    VkCommandBuffer cb = vk_cmd();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    if (set) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, sh->pl, 0, 1, &set, 0, NULL);
    if (sh->r.uses_rescaling) {   // a compute program reading scaled textures: their texel fetches and size queries in the game's pixels
        struct { uint32_t tex[4], img[2]; } rs;
        memcpy(rs.tex, g_rs_tex, sizeof rs.tex), memcpy(rs.img, g_rs_img, sizeof rs.img);
        vkCmdPushConstants(cb, sh->pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof rs, &rs);
    }
    if (dp->ind) {
        VkBuffer b; VkDeviceSize o;
        if (vk_buf_at(dp->ind, 12, &b, &o)) vkCmdDispatchIndirect(cb, b, o);
    } else vkCmdDispatch(cb, S.dispatch[0], S.dispatch[1], S.dispatch[2]);
    vk_barrier();
}

// ---------- clears and copies ----------
// the pass's area, cut to the scissor rectangle: clears honour the scissor, colour and depth / stencil alike (the
// depth / stencil clear used to clear the whole target)
static int clear_rect(VkClearRect *r) {
    *r = (VkClearRect){{{0, 0}, g_pass_ext}, 0, g_pass_layers};
    if (S.has_scissor) {
        int x = S.scissor[0], y = S.scissor[1], w = S.scissor[2], h = S.scissor[3];
        if (g_pass_scaled) { int x1 = vk_res_up_i(x + w), y1 = vk_res_up_i(y + h); x = vk_res_up_i(x), y = vk_res_up_i(y), w = x1 - x, h = y1 - y; }
        if (x < 0) w += x, x = 0;
        if (y < 0) h += y, y = 0;
        if (x + w > (int)g_pass_ext.width) w = (int)g_pass_ext.width - x;
        if (y + h > (int)g_pass_ext.height) h = (int)g_pass_ext.height - y;
        if (w <= 0 || h <= 0) return 0;
        r->rect = (VkRect2D){{x, y}, {(uint32_t)w, (uint32_t)h}};
    }
    return 1;
}
// Clears a colour target (inside the pass, as NVN's ClearColor does).
static void clear_color(const p_clear_color *c) {
    if (c->index < 0 || c->index >= 8 || !S.rt_on[c->index]) return;
    pass_begin();
    if ((uint32_t)c->index >= g_pass_ncol || !g_pass_ct[c->index]) return;
    if ((c->mask & 0xF) != 0xF) VRED_ONCE("a colour clear of some channels only (mask %#x): cleared whole", c->mask);
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)c->index};
    const vk_fmt *f = vk_format(S.rt_t[c->index].d.format);
    if (f && f->integer) for (int i = 0; i < 4; i++) { if (f->is_signed) a.clearValue.color.int32[i] = (int32_t)c->rgba[i]; else a.clearValue.color.uint32[i] = (uint32_t)c->rgba[i]; }
    else memcpy(a.clearValue.color.float32, c->rgba, 16);
    VkClearRect r;
    if (!clear_rect(&r)) return;
    vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
}
// Clears the depth and / or stencil target.
static void clear_ds(const p_clear_ds *c) {
    if (!S.ds_on) return;
    pass_begin();
    if (!g_pass_dt) return;
    VkClearAttachment a = {0, 0, {.depthStencil = {c->depth, (uint32_t)c->stencil}}};
    const vk_fmt *f = vk_format(S.ds_t.d.format);
    if (c->depth_mask && f && f->depth) a.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (c->stencil_mask && g_pass_has_stencil) a.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!a.aspectMask) return;
    VkClearRect r;
    if (!clear_rect(&r)) return;
    vkCmdClearAttachments(vk_cmd(), 1, &a, 1, &r);
}

// The Vulkan subresource, offset and extent of a copy region of a texture (and its view).
static void tex_region(vk_tex *x, const view_t *v, int has_v, const NVNcopyRegion *r, VkImageSubresourceLayers *sl, VkOffset3D *o, VkExtent3D *e) {
    unsigned lv = has_v && v->set_levels ? v->base_level : 0;
    unsigned l0 = has_v && v->set_layers ? v->min_layer : 0;
    int is3d = vk_tex_desc(x)->d.target == 2;
    VkImageAspectFlags asp = vk_tex_aspect(x);
    if (asp == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) asp = VK_IMAGE_ASPECT_DEPTH_BIT;
    *sl = (VkImageSubresourceLayers){asp, lv, is3d ? 0 : l0 + (uint32_t)r->zoffset, is3d ? 1 : (uint32_t)(r->depth > 0 ? r->depth : 1)};
    *o = (VkOffset3D){r->xoffset, r->yoffset, is3d ? r->zoffset : 0};
    *e = (VkExtent3D){(uint32_t)(r->width > 0 ? r->width : 1), (uint32_t)(r->height > 0 ? r->height : 1), is3d ? (uint32_t)(r->depth > 0 ? r->depth : 1) : 1};
}
// a region of x in the Vulkan image's pixels (x drawn at the resolution scale: the region scaled)
static void scale_box(vk_tex *x, VkOffset3D *o, VkExtent3D *e) {
    if (!vk_tex_scaled(x)) return;
    int32_t x0 = vk_res_up_i(o->x), y0 = vk_res_up_i(o->y), x1 = vk_res_up_i(o->x + (int32_t)e->width), y1 = vk_res_up_i(o->y + (int32_t)e->height);
    o->x = x0, o->y = y0, e->width = (uint32_t)(x1 - x0 > 0 ? x1 - x0 : 1), e->height = (uint32_t)(y1 - y0 > 0 ? y1 - y0 : 1);
}
// Copies guest memory into a texture.
static void copy_b2t(const p_copy_b2t *c) {
    vk_draw_suspend();
    vk_tex *x = vk_tex_get(c->tex, 0);
    if (!vk_tex_image(x)) return;
    if (vk_tex_scaled(x)) vk_tex_noscale(x, "copied into from memory");
    const vk_fmt *f = vk_format(c->tex->d.format);
    if (!f) return;
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e;
    tex_region(x, &c->view, c->has_view, &c->r, &sl, &o, &e);
    uint32_t bx = (e.width + f->bw - 1) / f->bw, by = (e.height + f->bh - 1) / f->bh;
    uint64_t row = S.copy_row_stride > 0 ? (uint64_t)S.copy_row_stride : (uint64_t)bx * f->bpb;
    uint64_t img = S.copy_img_stride > 0 ? (uint64_t)S.copy_img_stride : row * by;
    uint64_t n = img * (e.depth > 1 ? e.depth : sl.layerCount);
    VkBuffer b; VkDeviceSize off;
    if (f->decode == 1) {   // ASTC without GPU support: decoded on the CPU from the source rows
        uint8_t *lin = malloc((size_t)bx * by * 16), *rgba = malloc((size_t)e.width * e.height * 4);
        for (uint32_t y = 0; y < by; y++) memcpy(lin + (size_t)y * bx * 16, (const uint8_t *)(uintptr_t)c->src + y * row, (size_t)bx * 16);
        void nxl_astc_decode(const uint8_t *, size_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint8_t *);
        nxl_astc_decode(lin, (size_t)bx * by * 16, e.width, e.height, 1, f->bw, f->bh, rgba);
        void *d = vk_scratch((VkDeviceSize)e.width * e.height * 4, 16, &b, &off);
        memcpy(d, rgba, (size_t)e.width * e.height * 4);
        free(lin), free(rgba);
        VkBufferImageCopy bc = {off, 0, 0, sl, o, e};
        vkCmdCopyBufferToImage(vk_cmd(), b, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    } else {
        if (!vk_buf_at(c->src, n, &b, &off) || off % 4) {
            void *d = vk_scratch(n, 16, &b, &off);
            memcpy(d, (const void *)(uintptr_t)c->src, n);
        }
        VkBufferImageCopy bc = {off, (uint32_t)(row / f->bpb * f->bw), (uint32_t)(img / row * f->bh), sl, o, e};
        vkCmdCopyBufferToImage(vk_cmd(), b, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &bc);
    }
    vk_tex_mark_gpu(x);
    vk_barrier();
}
// Copies a texture into guest memory.
static void copy_t2b(const p_copy_b2t *c) {
    vk_draw_suspend();
    vk_tex *x = vk_tex_get(c->tex, 1);
    if (!vk_tex_image(x)) return;
    const vk_fmt *f = vk_format(c->tex->d.format);
    if (!f || f->decode) { VRED_ONCE("a copy out of a texture of format %#x (decoded on upload) is not supported", c->tex->d.format); return; }
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e;
    tex_region(x, &c->view, c->has_view, &c->r, &sl, &o, &e);
    uint32_t bx = (e.width + f->bw - 1) / f->bw, by = (e.height + f->bh - 1) / f->bh;
    uint64_t row = S.copy_row_stride > 0 ? (uint64_t)S.copy_row_stride : (uint64_t)bx * f->bpb;
    uint64_t img = S.copy_img_stride > 0 ? (uint64_t)S.copy_img_stride : row * by;
    VkBuffer b; VkDeviceSize off;
    if (!vk_buf_at(c->src, img * (e.depth > 1 ? e.depth : sl.layerCount), &b, &off)) { VRED_ONCE("a texture copied to memory outside every pool"); return; }
    VkImage from = vk_tex_image(x);
    if (vk_tex_scaled(x)) {   // drawn at the resolution scale: the region scaled down to the game's size first
        VkImage tmp = sl.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && sl.layerCount == 1 ? vk_tmp_image(vk_tex_format(x), e.width, e.height, 0) : VK_NULL_HANDLE;
        if (!tmp) { vk_tex_noscale(x, "copied to memory"); from = vk_tex_image(x); }
        else {
            VkOffset3D so = o; VkExtent3D se = e;
            scale_box(x, &so, &se);
            VkImageBlit bl = {sl, {so, {so.x + (int32_t)se.width, so.y + (int32_t)se.height, 1}}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(int32_t)e.width, (int32_t)e.height, 1}}};
            vkCmdBlitImage(vk_cmd(), from, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, f->integer ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            vk_barrier();
            from = tmp, sl = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, o = (VkOffset3D){0, 0, 0};
        }
    }
    VkBufferImageCopy bc = {off, (uint32_t)(row / f->bpb * f->bw), (uint32_t)(img / row * f->bh), sl, o, e};
    vkCmdCopyImageToBuffer(vk_cmd(), from, VK_IMAGE_LAYOUT_GENERAL, b, 1, &bc);
    vk_barrier();
}
// Copies between textures (a resolve, or a scaling blit when one is drawn at the resolution scale and the other is
// not).
static void copy_t2t(const p_copy_t2t *c) {
    vk_draw_suspend();
    vk_tex *s = vk_tex_get(c->src, 1), *d = vk_tex_get(c->dst, 0);
    if (!vk_tex_image(s) || !vk_tex_image(d)) return;
    vk_trace_tex("copy from", s, &c->sv, c->has_sv), vk_trace_tex("copy to", d, &c->dv, c->has_dv);
    if (vk_tex_scaled(s) != vk_tex_scaled(d) && (c->flags & DM_COPY_RESOLVE || !vk_tex_scaled(s))) {
        // a resolve needs both at one size: the destination follows the source where it can, else the scaled one goes
        // back to 1x; a plain copy between the two sizes is a scaling blit (from a scaled source the destination
        // follows it first, where it can, so the detail is kept)
        if (vk_tex_scaled(s) && vk_tex_res_eligible(d)) vk_tex_rescale(d, 1);
        else if (vk_tex_scaled(s)) vk_tex_rescale(s, 0);
        else if (c->flags & DM_COPY_RESOLVE) vk_tex_rescale(d, 0);
    }
    VkImageSubresourceLayers ssl, dsl; VkOffset3D so, dof; VkExtent3D se, de;
    tex_region(s, &c->sv, c->has_sv, &c->sr, &ssl, &so, &se);
    tex_region(d, &c->dv, c->has_dv, &c->dr, &dsl, &dof, &de);
    scale_box(s, &so, &se), scale_box(d, &dof, &de);
    VkCommandBuffer cb = vk_cmd();
    if (c->flags & DM_COPY_RESOLVE) {
        VkImageResolve r = {ssl, so, dsl, dof, de};
        vkCmdResolveImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        // NVN's copy converts from the source's format to the destination's (the 2D engine), scales and mirrors;
        // vkCmdCopyImage only moves bytes, and vkCmdBlitImage converts between the images' own formats. An image is shared by
        // the textures over its storage and keeps the first one's format, so a texture can mean another format than its
        // image holds: the blit then goes through an image of the meant format, copied as bytes. Breath of the Wild's
        // layout captures (the blurred scene behind menus and prompts) are RGB10A2 textures over storage first used as
        // RGBA8: blitted as RGBA8, the alpha bytes became the top of the blue channel
        VkFormat fs = vk_tex_nvn_format(s, &c->sv, c->has_sv), fd = vk_tex_nvn_format(d, &c->dv, c->has_dv);
        VkFormat is = vk_tex_format(s), id = vk_tex_format(d);
        int same_size = se.width == de.width && se.height == de.height && se.depth == de.depth;
        int mirror = c->flags & 0xC, convert = vk_copy_converts(fs, fd);
        int same_bpb = vk_format(c->src->d.format) && vk_format(c->dst->d.format) && vk_format(c->src->d.format)->bpb == vk_format(c->dst->d.format)->bpb;
        int color = ssl.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && dsl.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT;
        int blit = (!same_size || mirror || convert) && ssl.aspectMask == dsl.aspectMask && (!color || vk_blit_ok(fs, fd));
        if (!blit && !(same_size && same_bpb && ssl.aspectMask == dsl.aspectMask)) blit = 1;   // the old fallback
        if (convert && !blit) VRED_ONCE("texture copy between formats %d and %d copied as bytes (no blit for them)", fs, fd);
        if (!blit) {
            VkImageCopy r = {ssl, so, dsl, dof, se};
            vkCmdCopyImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        } else {
            VkImage simg = vk_tex_image(s), dimg = vk_tex_image(d);
            VkImageSubresourceLayers bsl = ssl, bdl = dsl;
            VkOffset3D bso = so, bdo = dof;
            int via_s = color && fs != is && se.depth == 1 && ssl.layerCount == 1, via_d = color && fd != id && de.depth == 1 && dsl.layerCount == 1;
            VkImageSubresourceLayers one = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
            VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
            if (via_s && (simg = vk_tmp_image(fs, se.width, se.height, 0))) {   // the source's bytes, in the format they are meant as
                VkImageCopy r = {ssl, so, one, {0, 0, 0}, se};
                vkCmdCopyImage(cb, vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, simg, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
                vkCmdPipelineBarrier2(cb, &di);
                bsl = one, bso = (VkOffset3D){0, 0, 0};
            } else if (!simg) simg = vk_tex_image(s);
            if (via_d && !(dimg = vk_tmp_image(fd, de.width, de.height, 1))) via_d = 0, dimg = vk_tex_image(d);
            if (via_d) bdl = one, bdo = (VkOffset3D){0, 0, 0};
            VkOffset3D d0 = bdo, d1 = {bdo.x + (int)de.width, bdo.y + (int)de.height, bdo.z + (int)de.depth};
            if (c->flags & 0x4) { int t = d0.x; d0.x = d1.x; d1.x = t; }   // MIRROR_X
            if (c->flags & 0x8) { int t = d0.y; d0.y = d1.y; d1.y = t; }   // MIRROR_Y
            VkImageBlit r = {bsl, {bso, {bso.x + (int)se.width, bso.y + (int)se.height, bso.z + (int)se.depth}}, bdl, {d0, d1}};
            vkCmdBlitImage(cb, simg, VK_IMAGE_LAYOUT_GENERAL, dimg, VK_IMAGE_LAYOUT_GENERAL, 1, &r,
                           (c->flags & 1) && color ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            if (via_d) {   // the converted texels, as bytes, into the destination image
                vkCmdPipelineBarrier2(cb, &di);
                VkImageCopy rc = {one, {0, 0, 0}, dsl, dof, de};
                vkCmdCopyImage(cb, dimg, VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &rc);
            }
            if (convert || via_s || via_d) { static int once; if (!once++) VLOG(1, "texture copies convert between formats (first: %d -> %d, images %d -> %d, %ux%u)", fs, fd, is, id, se.width, se.height); }
        }
    }
    vk_tex_mark_gpu(d);
    vk_barrier();
}
// Clears a region of a texture outside a pass.
static void clear_tex(const p_clear_tex *c) {
    vk_draw_suspend();
    vk_tex *x = vk_tex_get(c->tex, 0);
    if (!vk_tex_image(x)) return;
    VkImageSubresourceLayers sl; VkOffset3D o; VkExtent3D e;
    vk_trace_tex("clear", x, &c->view, c->has_view);
    tex_region(x, &c->view, c->has_view, &c->r, &sl, &o, &e);
    VkImageSubresourceRange rr = {sl.aspectMask, sl.mipLevel, 1, sl.baseArrayLayer, sl.layerCount};
    VkClearColorValue cc;
    memcpy(&cc, c->c.u, 16);
    // vkCmdClearColorImage clears whole layers in the image's format; NVN clears the region, in the texture's (or view's)
    // format. A part of a level (or of a 3D level's slices), or an image holding another format of the same size, is
    // cleared through an image of the right format and size, copied in as bytes per layer / slice
    VkFormat f = vk_tex_nvn_format(x, &c->view, c->has_view);
    uint32_t lw, lh, ld;
    vk_tex_extent(x, sl.mipLevel, &lw, &lh, &ld);
    int is3d = vk_tex_desc(x)->d.target == 2;
    int whole = o.x == 0 && o.y == 0 && e.width >= lw && e.height >= lh && (!is3d || (o.z == 0 && e.depth >= ld));
    scale_box(x, &o, &e);
    VkImage tmp = VK_NULL_HANDLE;
    const vk_fmt *nf = vk_format(c->tex->d.format);
    if (sl.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && nf && nf->bw == 1 && !nf->decode && (f != vk_tex_format(x) || !whole) && (tmp = vk_tmp_image(f, e.width, e.height, 1))) {
        VkCommandBuffer cb = vk_cmd();
        VkImageSubresourceRange tr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, tmp, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &tr);
        VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                               VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
        vkCmdPipelineBarrier2(cb, &di);
        uint32_t n = is3d ? e.depth : sl.layerCount;
        for (uint32_t k = 0; k < n; k++) {
            VkImageSubresourceLayers dl = sl;
            VkOffset3D dof = o;
            if (is3d) dof.z = o.z + (int)k; else dl.baseArrayLayer = sl.baseArrayLayer + k, dl.layerCount = 1;
            VkImageCopy r = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, dl, dof, {e.width, e.height, 1}};
            vkCmdCopyImage(cb, tmp, VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        }
    } else if (sl.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT) vkCmdClearColorImage(vk_cmd(), vk_tex_image(x), VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
    vk_tex_mark_gpu(x);
    vk_barrier();
}
// Copies between guest buffers: on the GPU when both are in memory pools, else through scratch or on the CPU.
static void copy_b2b(uint64_t src, uint64_t dst, uint64_t n) {
    if (!n) return;
    vk_draw_suspend();
    VkBuffer sb, db; VkDeviceSize so, dof;
    int s_in = vk_buf_at(src, n, &sb, &so), d_in = vk_buf_at(dst, n, &db, &dof);
    if (!d_in) { memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n); return; }   // CPU memory: at once
    if (!s_in || (sb == db && so < dof + n && dof < so + n)) {   // not in a pool, or overlapping: through scratch (the source as the CPU sees it now)
        void *t = vk_scratch(n, 16, &sb, &so);
        memcpy(t, (const void *)(uintptr_t)src, n);
    }
    VkBufferCopy r = {so, dof, n};
    vkCmdCopyBuffer(vk_cmd(), sb, db, 1, &r);
    vk_barrier();
}
// Uniform updates (nvnCommandBufferUpdateUniformBuffer) inside a render pass used to end it: a transfer cannot run in
// one. Breath of the Wild updates the first bytes of a uniform block before each draw group, which broke its frame
// into hundreds of passes (nxnative's notes count 220 of 386 outdoors), each a full barrier and an attachment store and
// load. Now an update inside a pass is held (pending) and copied at the pass's end, and a draw that binds a uniform range
// a held update covers reads a copy in scratch memory: the range as the CPU left it, with this submission's updates to
// it laid over in order (the applied ones are only in the GPU's copy). nxnative's ubp_get does the same
static void ub_apply(uint64_t dst, const void *data, uint64_t n, VkBuffer db, VkDeviceSize dof) {
    VkBuffer sb; VkDeviceSize so;
    void *t = vk_scratch(n, 16, &sb, &so);
    memcpy(t, data, n);
    VkBufferCopy r = {so, dof, n};
    vkCmdCopyBuffer(vk_cmd(), sb, db, 1, &r);
    (void)dst;
}
static void ub_note(uint64_t dst, const void *data, uint64_t n, int pending) {
    if (g_nupd == g_cupd) g_upd = realloc(g_upd, (size_t)(g_cupd = g_cupd ? 2 * g_cupd : 256) * sizeof *g_upd);
    g_upd[g_nupd++] = (ub_upd){dst, n, (const uint8_t *)data, pending};
    g_npending += pending;
}
static void ub_flush(void) {   // outside a render pass: the held updates, in order, into their buffers
    if (!g_npending) return;
    for (int i = 0; i < g_nupd; i++) {
        if (!g_upd[i].pending) continue;
        VkBuffer db; VkDeviceSize dof;
        if (vk_buf_at(g_upd[i].dst, g_upd[i].n, &db, &dof)) ub_apply(g_upd[i].dst, g_upd[i].data, g_upd[i].n, db, dof);
        g_upd[i].pending = 0;
    }
    g_npending = 0;
    vk_barrier();
}
// a uniform range a held update covers: its copy with the submission's updates laid over (0: no held update covers it)
static int ub_shadow(uint64_t addr, uint64_t size, VkDeviceSize align, VkDescriptorBufferInfo *out) {
    if (!g_npending) return 0;
    int hit = 0;
    for (int i = 0; i < g_nupd && !hit; i++) hit = g_upd[i].pending && g_upd[i].dst < addr + size && addr < g_upd[i].dst + g_upd[i].n;
    if (!hit) return 0;
    VkBuffer sb; VkDeviceSize so;
    uint8_t *d = vk_scratch(size, align, &sb, &so);
    memcpy(d, (const void *)(uintptr_t)addr, size);
    for (int i = 0; i < g_nupd; i++) {
        uint64_t lo = g_upd[i].dst > addr ? g_upd[i].dst : addr, hi = g_upd[i].dst + g_upd[i].n < addr + size ? g_upd[i].dst + g_upd[i].n : addr + size;
        if (lo < hi) memcpy(d + (lo - addr), g_upd[i].data + (lo - g_upd[i].dst), hi - lo);
    }
    *out = (VkDescriptorBufferInfo){sb, so, size};
    return 1;
}
// Writes into a uniform buffer from the command list (held until the pass ends when draws in it still read the old
// values).
static void update_ub(uint64_t dst, const void *data, uint64_t n) {
    VkBuffer db; VkDeviceSize dof;
    if (!vk_buf_at(dst, n, &db, &dof)) { memcpy((void *)(uintptr_t)dst, data, n); return; }
    if (g_pass_on && g_ub_hold) { ub_note(dst, data, n, 1); return; }   // held: copied at the pass's end
    vk_draw_suspend();
    ub_apply(dst, data, n, db, dof);
    vk_barrier();
    ub_note(dst, data, n, 0);
}
// Fills a guest buffer with a 32-bit value (on the GPU when it is in a memory pool).
static void fill_buf(uint64_t dst, uint64_t n, uint32_t v) {
    VkBuffer db; VkDeviceSize dof;
    if (!vk_buf_at(dst, n, &db, &dof) || dof % 4 || n % 4) {
        uint8_t *d = (uint8_t *)(uintptr_t)dst;
        for (uint64_t i = 0; i + 4 <= n; i += 4) memcpy(d + i, &v, 4);
        return;
    }
    vk_draw_suspend();
    vkCmdFillBuffer(vk_cmd(), db, dof, n, v);
    vk_barrier();
}

// ---------- the replay ----------
static void set_rt(const p_set_rt *r) {
    vk_draw_suspend();
    S.nrt = r->n > 8 ? 8 : r->n;
    for (int i = 0; i < 8; i++) {
        S.rt_on[i] = i < S.nrt && r->color[i];
        if (S.rt_on[i]) S.rt_t[i] = *r->color[i], S.rt_v[i] = r->cview[i], S.rt_has_v[i] = r->has_cview[i];
    }
    S.ds_on = r->depth != NULL;
    if (S.ds_on) S.ds_t = *r->depth, S.ds_v = r->dview, S.ds_has_v = r->has_dview;
}

void vk_draw_reset(void) {
    g_pass_on = 0, g_cut_due = 0, g_seg_draws = 0, g_nupd = 0, g_npending = 0;
    static int init;
    if (!init) { init = 1; const char *e = getenv("NXL_UB_HOLD"); g_ub_hold = !(e && !strcmp(e, "0")); }
}
void vk_draw_end(void) { vk_draw_suspend(); vk_tex_writeback(vk_cmd()); }

// Runs a recorded NVN command list: each command turned into Vulkan commands (nested lists followed up to 16 deep).
void vk_exec_list(const uint8_t *p, int depth) {
    if (depth > 16) { VRED("command lists nested more than 16 deep"); return; }
    for (;;) {
        op_hdr h;
        memcpy(&h, p, sizeof h);
        const uint8_t *a = p + 8;
        VLOG(3, "op %u size %u", h.op, h.size);
        if (g_cut_due && !g_pass_on) { g_cut_due = 0, g_seg_draws = 0; vk_segment_cut(); }
        switch (h.op) {
        case OP_END: return;
        case OP_JUMP: memcpy(&p, a, 8); continue;
        case OP_CALL: { uint64_t t; memcpy(&t, a, 8); vk_exec_list((const uint8_t *)(uintptr_t)t, depth + 1); break; }
        case OP_SIGNAL_EVENT: {
            volatile uint32_t *val; uint32_t mode, v;
            memcpy(&val, a, 8), memcpy(&mode, a + 8, 4), memcpy(&v, a + 12, 4);
            vk_post_event(val, mode, v);
            break;
        }
        case OP_REPORT_COUNTER: {
            uint64_t t, addr;
            memcpy(&t, a, 8), memcpy(&addr, a + 8, 8);
            if (t == 1 && g_occ_on) occ_report(addr);   // SAMPLES_PASSED
            else vk_post_counter(addr, (uint32_t)t);
            break;
        }
        case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == 1) occ_reset(); break; }
        case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = vk_next_fence(); s->fenced = 1; break; }
        case OP_COPY_B2B: { uint64_t src, dst, n; memcpy(&src, a, 8), memcpy(&dst, a + 8, 8), memcpy(&n, a + 16, 8); copy_b2b(src, dst, n); break; }
        case OP_UPDATE_UB: { uint64_t dst, n; memcpy(&dst, a, 8), memcpy(&n, a + 8, 8); update_ub(dst, a + 16, n); break; }
        case OP_CLEAR_BUF: { const p_clear_buf *c = (const p_clear_buf *)a; fill_buf(c->addr, c->size, c->value); break; }
        case OP_SET_RT: set_rt((const p_set_rt *)a); break;
        case OP_CLEAR_COLOR: clear_color((const p_clear_color *)a); break;
        case OP_CLEAR_DS: clear_ds((const p_clear_ds *)a); break;
        case OP_VIEWPORTS: {
            const p_viewports *v = (const p_viewports *)a;
            for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], 6 * sizeof(float));
            break;
        }
        case OP_SCISSOR: { const p_scissor *s = (const p_scissor *)a; S.scissor[0] = s->x, S.scissor[1] = s->y, S.scissor[2] = s->w, S.scissor[3] = s->h, S.has_scissor = 1; break; }
        case OP_DEPTH_RANGE: { const p_depth_range *d = (const p_depth_range *)a; S.depth_n = d->n, S.depth_f = d->f; break; }
        case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; break;
        case OP_BIND_STATE: {
            const p_bind_state *b = (const p_bind_state *)a;
            switch (b->kind) {
            case 0: { blend_st bs; memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S.blend[bs.target] = bs; break; }
            case 1: memcpy(&S.color, b->bytes, sizeof S.color); break;
            case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
            case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break;
            case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break;
            case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break;
            }
            break;
        }
        case OP_BIND_VATTR: { const p_bind_vattr *v = (const p_bind_vattr *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vattr[v->first + i] = v->st[i]; break; }
        case OP_BIND_VSTREAM: { const p_bind_vstream *v = (const p_bind_vstream *)a; for (int i = 0; i < v->n && v->first + i < 16; i++) S.vstream[v->first + i] = v->st[i]; break; }
        case OP_BIND_VBUF: case OP_BIND_VBUF1: {
            p_bind_vbuf t;
            const p_bind_vbuf *v = vbuf_rec(h.op, a, &t);
            for (int i = 0; i < v->n && v->first + i < 16; i++) S.vbuf[v->first + i] = (range){v->r[i].address, v->r[i].size};
            break;
        }
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: {
            p_bind_buf t;
            const p_bind_buf *b = bind_buf_rec(h.op, a, &t);
            if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 18) S.ubo[b->stage][b->index] = (range){b->addr, b->size};
            break;
        }
        case OP_BIND_SSBO: { const p_bind_buf *b = (const p_bind_buf *)a; if (b->stage >= 0 && b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = (range){b->addr, b->size}; break; }
        case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: {
            p_bind_handles t;
            const p_bind_handles *b = h.op == OP_BIND_TEX1 ? bind_rec(h.op, a, &t) : (const p_bind_handles *)a;
            if (b->stage < 0 || b->stage >= 6) break;
            for (int i = 0; i < b->n && i < 32; i++) {   // the stage's c2 is rebuilt only when a handle changes
                uint64_t v = b->h[i], *slot = NULL;
                if (b->defer && b->src) v = ((const uint64_t *)(uintptr_t)b->src)[i];
                int s = b->first + i;
                if (h.op == OP_BIND_SAMP) { if (s < 128) slot = &S.samp[b->stage][s]; }
                else if (h.op == OP_BIND_IMG) { if (s < 8) slot = &S.img[b->stage][s]; }
                else if (s < NTEX) slot = &S.tex[b->stage][s];
                if (slot && *slot != v) *slot = v, g_c2_dirty[b->stage] = 1;
            }
            break;
        }
        case OP_STENCIL: {
            const p_stencil *s = (const p_stencil *)a;
            for (int f = 0; f < 2; f++) {
                if (!(s->face & (1 << f))) continue;
                if (s->which == 0) S.st_vmask[f] = (uint32_t)s->value_mask;
                else if (s->which == 1) S.st_wmask[f] = (uint32_t)s->mask;
                else S.st_ref[f] = (uint32_t)s->ref;
            }
            break;
        }
        case OP_POLY_OFFSET: { const p_poly_offset *o = (const p_poly_offset *)a; S.poly_factor = o->factor, S.poly_units = o->units, S.poly_clamp = o->clamp; break; }
        case OP_DRAW: { p_draw t; const p_draw *d = draw_rec(a, h.size, &t); draw_common(d->prim, 0, d, NULL); break; }
        case OP_DRAW_IDX: { p_draw_idx t; const p_draw_idx *d = draw_idx_rec(a, h.size, &t); draw_common(d->prim, 1, NULL, d); break; }
        case OP_DISPATCH: dispatch((const p_dispatch *)a); break;
        case OP_BARRIER: vk_draw_suspend(); vk_barrier(); break;
        case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;   // handles already carry the pools' bases
        case OP_COPY_B2T: copy_b2t((const p_copy_b2t *)a); break;
        case OP_COPY_T2B: copy_t2b((const p_copy_b2t *)a); break;
        case OP_COPY_T2T: copy_t2t((const p_copy_t2t *)a); break;
        case OP_COPY_STRIDE: { const p_copy_stride *c = (const p_copy_stride *)a; if (c->image) S.copy_img_stride = c->stride; else S.copy_row_stride = c->stride; break; }
        case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
        case OP_CLEAR_TEX: clear_tex((const p_clear_tex *)a); break;
        case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_depth_clamp *)a)->on; break;
        case OP_RENDER_COND: VRED_ONCE("conditional rendering: the host draws anyway"); break;
        case OP_TF_BIND: case OP_TF_STATE: VRED_ONCE("transform feedback is not supported yet"); break;
        case OP_RAST_DISCARD: S.rast_discard = ((const p_rast_discard *)a)->on; break;
        case OP_VP_SWIZZLE: {   // not applied: report a non-identity one (NVNviewportSwizzle XOR identity, so 0 is unchanged)
            const p_vp_swizzle *v = (const p_vp_swizzle *)a;
            for (int i = 0; i < v->n && i < 16; i++)
                if (v->sw[i][0] | v->sw[i][1] | v->sw[i][2] | v->sw[i][3]) { VRED_ONCE("viewport swizzles (%d %d %d %d) are not applied", v->sw[i][0], v->sw[i][1], v->sw[i][2], v->sw[i][3]); break; }
            break;
        }
        case OP_PRIM_RESTART: { const p_prim_restart *r = (const p_prim_restart *)a; S.prim_restart = (int)r->on; S.prim_restart_idx = r->index; break; }
        default: VRED_ONCE("command record %u is not replayed", h.op); break;
        }
        if (h.size < 8) { VRED("a command record of %u bytes (op %u)", h.size, h.op); return; }
        p += h.size;
    }
}

void vk_draw_stats(uint64_t *passes, uint64_t *draws, uint64_t *pipes, uint64_t *translations, uint64_t *fails) {
    *passes = g_stat_passes, *draws = g_stat_draws, *pipes = g_stat_pipelines, *translations = g_stat_translations, *fails = g_stat_translate_fail;
}
