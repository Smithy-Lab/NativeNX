// SPDX-License-Identifier: GPL-3.0-or-later
// nxlinux's Vulkan backend for nxnative's NVN layer: the replay interface nxnative's Metal backend provides (nvn_mtl.h,
// nvn_obj.h), on Vulkan. Internal declarations shared by the backend's files:
//   vk_core.c     instance, device, queue, memory, the game's memory pools as imported host memory, submissions
//   vk_tex.c      formats, textures (Vulkan images; block-linear guest data deswizzled on upload), samplers, the
//                 texture and sampler pools' registrations
//   vk_draw.c     the command records' replay: state, render passes, pipelines (shaders via nxl_shader.cpp),
//                 descriptors, draws, dispatches, copies and clears
//   vk_present.c  the window (SDL3) and presentation; without a window, presents are counted and can be dumped
#pragma once
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#include "nvn_obj.h"
#include "perf.h"
void vk_draw_stats(uint64_t *passes, uint64_t *draws, uint64_t *pipes, uint64_t *translations, uint64_t *fails);
void vk_pcache_save_now(void);   // the pipeline cache to disk (at exit)

extern int g_vk_log;   // NXL_VK_LOG: 1 = per-frame notes, 2 = every command
#define VLOG(lvl, ...) do { if (g_vk_log >= (lvl)) { fprintf(stderr, "[vk] " __VA_ARGS__); fputc('\n', stderr); } } while (0)
#define VRED(...) do { fprintf(stderr, "[vk] [RED] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#define VRED_ONCE(...) do { static int once_; if (!once_++) VRED(__VA_ARGS__); } while (0)
#define VK_CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) VRED("%s failed: %d (%s:%d)", #x, r_, __FILE__, __LINE__); } while (0)

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t qfam;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    VkPhysicalDeviceFeatures feat;
    uint32_t api;
    uint64_t host_ptr_align;              // minImportedHostPointerAlignment
    uint8_t has_host_import, has_custom_border, has_swapchain, has_depth_clip, has_shader_int8, has_storage8,
        has_storage16, has_float16, has_vertex_div, nvidia, has_d24s8, has_astc, has_output_layer;
    VkPipelineCache pcache;
    VkQueryPool occ_pool;      // occlusion queries (NVN's SAMPLES_PASSED counter), reset at each submission's start
    VkCommandPool cpool;
    VkSampler nearest;                    // the blit sampler
} vk_ctx;
extern vk_ctx g_vk;

int vk_ready(void);                       // the device is up (vk_init ran and succeeded)
uint32_t vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want);

// ---------- memory pools (vk_core.c) ----------
// the buffer and offset holding [addr, addr + len): the game's memory, imported (GPU address = CPU address)
int vk_buf_at(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off);

// ---------- the current submission (vk_core.c) ----------
VkCommandBuffer vk_cmd(void);             // the submission's command buffer (begun at mtl_sub_begin)
// transient memory for this submission (uniform data, staging): host-visible, freed when it completes
void *vk_scratch(VkDeviceSize n, VkDeviceSize align, VkBuffer *buf, VkDeviceSize *off);
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout l);   // from this submission's descriptor pool
void vk_defer_destroy_image(VkImage im, VkImageView *views, int nviews, VkDeviceMemory m);
void vk_barrier(void);                    // a full memory barrier in the command stream (between passes, copies, dispatches)
// one-off work outside a submission (texture uploads at registration): runs and completes before it returns
VkCommandBuffer vk_once_begin(void);
void vk_once_end(VkCommandBuffer cb);

// ---------- formats and textures (vk_tex.c) ----------
typedef struct {
    VkFormat vk;                          // VK_FORMAT_UNDEFINED: no counterpart
    uint8_t bpb, bw, bh;                  // bytes per block, block size (1x1 uncompressed)
    uint8_t depth, stencil, integer, is_signed, srgb, decode;   // decode: 1 = ASTC decoded to RGBA8 on upload, 2 = 3-component expanded to 4
    VkComponentSwizzle swz[4];
} vk_fmt;
const vk_fmt *vk_format(int nvn_format);
VkFormat vk_vertex_format(int nvn_format);   // a vertex attribute's format
int vk_fmt_yuzu_pixel(int nvn_format);       // yuzu TexturePixelFormat (typeless image loads), -1 unknown

typedef struct vk_tex vk_tex;
// the host image for a texture object (made on first use, by serial); upload = bring sampled CPU-written contents up to date
vk_tex *vk_tex_get(const tex_t *t, int upload);
VkImage vk_tex_image(vk_tex *x);
VkFormat vk_tex_format(vk_tex *x);   // the Vulkan image's format
VkFormat vk_tex_nvn_format(vk_tex *x, const view_t *v, int has_view);   // the format the game means (an image can hold several)
uint32_t vk_tex_view_layers(void);   // layers of the view vk_tex_view returned last, on this thread
VkImageView vk_tex_storage_view(vk_tex *x, const view_t *v, int has_view, int img_format);
int vk_trace_wants(vk_tex *x, const view_t *v, int has_view);
uint64_t vk_presents(void);
int vk_segment_cut(void);            // submit what is recorded and go on in a new command buffer (no render pass open)
uint64_t vk_stat_segments(void);
void vk_tex_writeback(VkCommandBuffer cb);   // copy GPU-written linear textures back to their storage (submission end)
int vk_occ_alloc(void);                        // a query of occ_pool for this submission, -1 when they run out
void vk_post_occlusion(uint64_t addr, uint64_t base, const uint32_t *idx, uint32_t n);   // write base + those queries' samples
void vk_occ_fold(void);                        // vk_draw.c: after a submission, its queries' samples into the running count
int vk_hide_take_dump(char *path, size_t n);   // the hide cycle's next picture, if one is due   // frames presented so far
void vk_trace_dump_later(const tex_t *t);
void vk_trace_dump_flush(void);
void vk_trace_tex(const char *op, vk_tex *x, const view_t *v, int has_view);
VkImage vk_tmp_image(VkFormat f, uint32_t w, uint32_t h, int slot);
int vk_copy_converts(VkFormat s, VkFormat d);
int vk_blit_ok(VkFormat s, VkFormat d);
VkImageView vk_tex_view(vk_tex *x, const view_t *v, int has_view, int for_storage, int for_attachment, int stencil);
void vk_tex_mark_gpu(vk_tex *x);          // rendered to / written by a shader or a copy: GPU contents, never re-uploaded
void vk_tex_extent(vk_tex *x, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d);   // the texture's size (the game's)
// ---------- resolution scale (vk_tex.c: NXL_RES_SCALE) ----------
int vk_res_active(void);
void vk_res_params(uint32_t *up, uint32_t *shift);   // the scale as up / 2^shift
uint32_t vk_res_up(uint32_t v);                       // a size or coordinate at the scale
int32_t vk_res_up_i(int32_t v);
float vk_res_factor(void);
int vk_tex_scaled(vk_tex *x);
void vk_tex_hextent(vk_tex *x, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d);   // the Vulkan image's size
int vk_tex_res_eligible(vk_tex *x);
int vk_tex_rescale(vk_tex *x, int up);
void vk_tex_noscale(vk_tex *x, const char *why);
extern atomic_uint vk_tex_gen;
uint32_t vk_tex_layers(vk_tex *x);
const tex_t *vk_tex_desc(vk_tex *x);
VkImageAspectFlags vk_tex_aspect(vk_tex *x);
// texture / sampler pool registrations (mtl_reg_tex / mtl_reg_samp): what a handle names
typedef struct {
    tex_t t; view_t v; uint8_t used, image;
    // the sampled view this entry gave in submission csub (write_descriptors): a texture is uploaded and its view looked
    // up once per submission, not for every draw that samples it
    uint64_t csub; void *cx; VkImageView cview; uint8_t cgpu, cscaled; unsigned cgen;   // cgpu: GPU-drawn (no forced anisotropy); cscaled: at the resolution scale; cgen: vk_tex_gen then
} vk_reg_tex;
VkSampler vk_reg_sampler(uint32_t id);   // the sampler registered as id, made once per registration
VkSampler vk_reg_sampler_for(uint32_t id, int plain);   // plain: without forced anisotropic filtering (a GPU-drawn texture)
int vk_tex_gpu_drawn(vk_tex *x);
const vk_reg_tex *vk_reg_tex_at(uint32_t id);
const samp_par *vk_reg_samp_at(uint32_t id);
VkSampler vk_sampler(const samp_par *p);
int vk_tex_type_of(const tex_t *t, const view_t *v, int has_view);   // NXL_TT_*
void vk_tex_reap(void);                   // after a submission: free textures released two submissions ago
void vk_tex_flush_uploads(void);          // before a submission's replay: CPU-written textures it may sample

// ---------- presentation (vk_present.c) ----------
void vk_present_init(void);
void vk_present_tex(const tex_t *t, const int crop[4]);   // blit the texture to the window (or dump it); waits for the GPU

// ---------- replay (vk_draw.c) ----------
void vk_draw_reset(void);                 // submission start: draw state back to NVN's defaults
void vk_draw_end(void);                   // submission end: close an open render pass
void vk_exec_list(const uint8_t *p, int depth);
void vk_draw_init(void);
