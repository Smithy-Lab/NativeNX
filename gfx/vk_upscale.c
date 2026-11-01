// SPDX-License-Identifier: GPL-3.0-or-later
// The game's frame upscaled to the window (vk_present.c): AMD FidelityFX Super Resolution 1.0 (third_party/fsr, MIT), its
// edge-adaptive upscaling (EASU) and then its sharpening (RCAS), as two compute passes into images of the size the frame is
// shown at; vk_present.c then copies the result 1:1 into the window. The game still renders at its own size (Breath of the
// Wild docked: up to 1600x900); this makes the step to a 1080p, 1440p or 4K screen sharper than the plain bilinear stretch.
//   NXL_UPSCALE    fsr (the default), linear (bilinear, as before) or nearest (sharp pixels)
//   NXL_SHARPNESS  RCAS's sharpening in stops below its strongest: 0 strongest, 0.25 the default, 2 light; "off" skips RCAS
// Frames shown smaller than the game's size (a small window) are scaled down by the bilinear blit whatever the setting.
#include "vk.h"
#include <math.h>
#include <stdlib.h>
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#include "shaders/shaders_spv.h"

void vk_idle(void);
int vk_upscale_mode(void);
int vk_upscale(VkCommandBuffer cb, uint32_t slot, vk_tex *x, int sx, int sy, int sw, int sh, int flip, uint32_t dw, uint32_t dh, VkImage *out);

enum { UP_LINEAR, UP_FSR, UP_NEAREST };
static struct {
    int mode, init, failed, rcas;
    float sharp;
    VkSampler smp;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkPipeline pipe[2];   // EASU, RCAS
    VkDescriptorPool dp;
    VkDescriptorSet ds[8][2];
    VkImage img[2];
    VkDeviceMemory mem[2];
    VkImageView view[2];
    uint32_t w, h;
} U = {.mode = -1};

int vk_upscale_mode(void) {   // 0 bilinear, 1 FSR, 2 nearest
    if (U.mode < 0) {
        const char *e = getenv("NXL_UPSCALE"), *s = getenv("NXL_SHARPNESS");
        U.mode = !e || !*e || !strcmp(e, "fsr") ? UP_FSR : !strcmp(e, "nearest") ? UP_NEAREST : UP_LINEAR;
        if (e && *e && strcmp(e, "fsr") && strcmp(e, "linear") && strcmp(e, "nearest")) VRED("NXL_UPSCALE=%s: fsr, linear or nearest; using linear", e);
        U.rcas = !(s && !strcmp(s, "off"));
        U.sharp = 0.25f;
        if (s && U.rcas) { char *end; float v = strtof(s, &end); if (end != s && !*end && v >= 0 && v <= 4) U.sharp = v; else VRED("NXL_SHARPNESS=%s: 0 (strongest) to 4, or off", s); }
        fprintf(stderr, "[vk] the frame is scaled to the window by %s\n", U.mode == UP_FSR ? (U.rcas ? "FSR 1.0 (upscaling and sharpening)" : "FSR 1.0 (upscaling, no sharpening)")
                                                                           : U.mode == UP_NEAREST ? "nearest pixels" : "bilinear filtering");
    }
    return U.mode;
}

// A shader module from SPIR-V words.
static VkShaderModule module(const uint32_t *w, size_t n) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n * 4, w};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(g_vk.dev, &ci, NULL, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
    return m;
}
// Sets up FSR 1.0 (its sampler, layouts and the EASU and RCAS pipelines) once; 1 when it is ready.
static int up_init(void) {
    if (U.init) return !U.failed;
    U.init = 1, U.failed = 1;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    sci.maxLod = 0;
    if (vkCreateSampler(g_vk.dev, &sci, NULL, &U.smp) != VK_SUCCESS) return 0;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, b};
    if (vkCreateDescriptorSetLayout(g_vk.dev, &dci, NULL, &U.dsl) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo pci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &U.dsl, 1, &pr};
    if (vkCreatePipelineLayout(g_vk.dev, &pci, NULL, &U.pl) != VK_SUCCESS) return 0;
    const uint32_t *code[2] = {spv_fsr_easu, spv_fsr_rcas};
    size_t len[2] = {sizeof spv_fsr_easu / 4, sizeof spv_fsr_rcas / 4};
    for (int i = 0; i < 2; i++) {
        VkShaderModule m = module(code[i], len[i]);
        if (!m) return 0;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
                                          {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", NULL}, U.pl};
        VkResult r = vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &ci, NULL, &U.pipe[i]);
        vkDestroyShaderModule(g_vk.dev, m, NULL);
        if (r != VK_SUCCESS) return 0;
    }
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 16}};
    VkDescriptorPoolCreateInfo dpi = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 16, 2, ps};
    if (vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &U.dp) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout ls[16];
    for (int i = 0; i < 16; i++) ls[i] = U.dsl;
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, U.dp, 16, ls};
    if (vkAllocateDescriptorSets(g_vk.dev, &ai, &U.ds[0][0]) != VK_SUCCESS) return 0;
    U.failed = 0;
    return 1;
}
// the two images the passes write, of the shown size (made again when the window's size changes)
static int up_images(uint32_t w, uint32_t h) {
    if (U.img[0] && U.w == w && U.h == h) return 1;
    if (U.img[0]) {
        vk_idle();   // earlier frames in flight may still read them
        for (int i = 0; i < 2; i++) vkDestroyImageView(g_vk.dev, U.view[i], NULL), vkDestroyImage(g_vk.dev, U.img[i], NULL), vkFreeMemory(g_vk.dev, U.mem[i], NULL);
        memset(U.img, 0, sizeof U.img);
    }
    for (int i = 0; i < 2; i++) {
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        if (vkCreateImage(g_vk.dev, &ci, NULL, &U.img[i]) != VK_SUCCESS) { U.img[i] = VK_NULL_HANDLE; return 0; }
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g_vk.dev, U.img[i], &mr);
        VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        if (vkAllocateMemory(g_vk.dev, &mai, NULL, &U.mem[i]) != VK_SUCCESS || vkBindImageMemory(g_vk.dev, U.img[i], U.mem[i], 0) != VK_SUCCESS) return 0;
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, U.img[i], VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0},
                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        if (vkCreateImageView(g_vk.dev, &vi, NULL, &U.view[i]) != VK_SUCCESS) return 0;
    }
    U.w = w, U.h = h;
    return 1;
}
static void set_desc(VkDescriptorSet s, VkImageView src, VkImageView dst) {
    VkDescriptorImageInfo a = {U.smp, src, VK_IMAGE_LAYOUT_GENERAL}, b = {VK_NULL_HANDLE, dst, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &a},
                                 {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, s, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &b}};
    vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
}
static void barrier(VkCommandBuffer cb, VkImage im, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da, int discard) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da, discard ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL,
                               VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}
static float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// Records the passes for the frame's crop rectangle (sx, sy, sw, sh of x, upside down when flip) shown at dw x dh, into
// command buffer cb of frame slot slot (0-7; its descriptor sets are free once the slot's fence passed). *out: the image
// holding the result (layout GENERAL, ready to be read by a transfer). 0: not done (the caller blits as before)
static int upscale(VkCommandBuffer cb, uint32_t slot, VkImageView src, VkFormat f, uint32_t tw, uint32_t th, int sx, int sy, int sw, int sh, int flip,
                   uint32_t dw, uint32_t dh, VkImage *out);
// Scales the frame up to the window with FSR 1.0 (EASU, then RCAS sharpening); 0 when it does not apply (not upscaling,
// or FSR off).
int vk_upscale(VkCommandBuffer cb, uint32_t slot, vk_tex *x, int sx, int sy, int sw, int sh, int flip, uint32_t dw, uint32_t dh, VkImage *out) {
    if (vk_upscale_mode() != UP_FSR || (int)dw <= sw || (int)dh <= sh || slot >= 8) return 0;   // only upscaling
    if (!up_init()) { VRED_ONCE("FSR could not be set up: the frame is scaled by bilinear filtering"); return 0; }
    VkImageView src = vk_tex_view(x, NULL, 0, 0, 0, 0);
    if (!src) return 0;
    uint32_t tw, th, td;
    vk_tex_hextent(x, 0, &tw, &th, &td);
    return upscale(cb, slot, src, vk_tex_format(x), tw, th, sx, sy, sw, sh, flip, dw, dh, out);
}
// the same for an image of the present's own (a frame an OpenGL game drew, vk_present.c): its view, format and size
int vk_upscale_view(VkCommandBuffer cb, uint32_t slot, VkImageView src, VkFormat f, uint32_t tw, uint32_t th, int sx, int sy, int sw, int sh, int flip,
                    uint32_t dw, uint32_t dh, VkImage *out);
int vk_upscale_view(VkCommandBuffer cb, uint32_t slot, VkImageView src, VkFormat f, uint32_t tw, uint32_t th, int sx, int sy, int sw, int sh, int flip,
                    uint32_t dw, uint32_t dh, VkImage *out) {
    if (vk_upscale_mode() != UP_FSR || (int)dw <= sw || (int)dh <= sh || slot >= 8 || !src) return 0;
    if (!up_init()) { VRED_ONCE("FSR could not be set up: the frame is scaled by bilinear filtering"); return 0; }
    return upscale(cb, slot, src, f, tw, th, sx, sy, sw, sh, flip, dw, dh, out);
}
// Records FSR's two passes from the source view into the output image at the window's size.
static int upscale(VkCommandBuffer cb, uint32_t slot, VkImageView src, VkFormat f, uint32_t tw, uint32_t th, int sx, int sy, int sw, int sh, int flip,
                   uint32_t dw, uint32_t dh, VkImage *out) {
    if (!up_images(dw, dh)) return 0;
    int srgb = f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    // the game's writes to the frame, then the passes
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
    vkCmdPipelineBarrier2(cb, &di);
    for (int i = 0; i < 2; i++) barrier(cb, U.img[i], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, 1);
    // EASU: the crop rectangle into img[0]
    struct { uint32_t con[4][4]; uint32_t size[2], flip, srgb; } pe;
    FsrEasuCon(pe.con[0], pe.con[1], pe.con[2], pe.con[3], (AF1)sw, (AF1)sh, (AF1)tw, (AF1)th, (AF1)dw, (AF1)dh);
    pe.con[0][2] = f2u(u2f(pe.con[0][2]) + (float)sx);   // the rectangle's corner (FSR's viewport starts at 0, 0)
    pe.con[0][3] = f2u(u2f(pe.con[0][3]) + (float)sy);
    pe.size[0] = dw, pe.size[1] = dh, pe.flip = flip != 0, pe.srgb = srgb;
    set_desc(U.ds[slot][0], src, U.view[0]);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pipe[0]);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pl, 0, 1, &U.ds[slot][0], 0, NULL);
    vkCmdPushConstants(cb, U.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pe, &pe);
    vkCmdDispatch(cb, (dw + 7) / 8, (dh + 7) / 8, 1);
    VkImage res = U.img[0];
    if (U.rcas) {   // RCAS: img[0] sharpened into img[1]
        barrier(cb, U.img[0], VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, 0);
        struct { uint32_t con[4]; uint32_t size[2]; } pr;
        FsrRcasCon(pr.con, U.sharp);
        pr.size[0] = dw, pr.size[1] = dh;
        set_desc(U.ds[slot][1], U.view[0], U.view[1]);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pipe[1]);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pl, 0, 1, &U.ds[slot][1], 0, NULL);
        vkCmdPushConstants(cb, U.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pr, &pr);
        vkCmdDispatch(cb, (dw + 7) / 8, (dh + 7) / 8, 1);
        res = U.img[1];
    }
    barrier(cb, res, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, 0);
    *out = res;
    return 1;
}

// ---------- self-test (NXL_TEST_UPSCALE=<folder>, tests/gfx/t_upscale.py) ----------
// A test picture (hard edges at several angles, thin lines, a gradient) upscaled 2.25x by FSR and by a CPU bilinear
// reference; both written as PPM pictures with the source, and the numbers t_upscale.py checks printed: the pictures must
// agree away from edges, and FSR's edges must be steeper than bilinear's
static uint8_t pat(int x, int y, int c) {
    int v;
    if (y < 40) v = (x / 8 + y / 8) & 1 ? 230 : 25;                            // checkerboard
    else if (y < 80) v = (x * 2 + (y - 40) * 3) % 50 < 25 ? 220 : 30;          // slanted stripes
    else if (y < 100) v = x % 7 == 0 ? 240 : 40;                                // thin lines
    else v = x * 255 / 159;                                                     // a gradient
    return (uint8_t)(c == 1 ? v : c == 0 ? (v * 3 + 40) / 4 : 255 - v / 2);
}
// Writes an RGBA picture as a PPM (for the self-test).
static void write_ppm(const char *path, const uint8_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) fwrite(rgba + 4 * i, 1, 3, f);
    fclose(f);
}
void vk_upscale_selftest(const char *dir);
// Upscales a test picture with FSR and writes the result, for the tests.
void vk_upscale_selftest(const char *dir) {
    enum { SW = 160, SH = 120 };
    const uint32_t DW = 360, DH = 270;
    U.mode = UP_FSR; U.rcas = 1; U.sharp = 0.25f;
    if (!up_init()) { fprintf(stderr, "[vk] upscale self-test: FSR could not be set up\n"); return; }
    // the source picture, uploaded through a host buffer
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {SW, SH, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage im; VkDeviceMemory imem; VkImageView iv;
    VK_CHECK(vkCreateImage(g_vk.dev, &ci, NULL, &im));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, im, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &mai, NULL, &imem));
    VK_CHECK(vkBindImageMemory(g_vk.dev, im, imem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &iv));
    VkDeviceSize bsz = (VkDeviceSize)DW * DH * 4;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bsz, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VkDeviceMemory bm; uint8_t *p;
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &b));
    vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
    VkMemoryAllocateInfo bai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &bai, NULL, &bm));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, b, bm, 0));
    VK_CHECK(vkMapMemory(g_vk.dev, bm, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    static uint8_t src[SW * SH * 4];
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++) for (int c = 0; c < 4; c++) src[4 * (y * SW + x) + c] = c == 3 ? 255 : pat(x, y, c);
    memcpy(p, src, sizeof src);
    VkCommandBuffer cb = vk_once_begin();
    VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im,
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
    vkCmdPipelineBarrier2(cb, &di);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {SW, SH, 1}};
    vkCmdCopyBufferToImage(cb, b, im, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    VkImage out;
    int ok = upscale(cb, 0, iv, VK_FORMAT_R8G8B8A8_UNORM, SW, SH, 0, 0, SW, SH, 0, DW, DH, &out);
    if (ok) {
        VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {DW, DH, 1}};
        vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    }
    vk_once_end(cb);
    if (!ok) { fprintf(stderr, "[vk] upscale self-test: the passes were not recorded\n"); return; }
    // the CPU's bilinear reference (pixel centres, as the GPU's blit), and the comparison
    uint8_t *ref = malloc(bsz);
    double flat = 0, edge_fsr = 0, edge_lin = 0; long nflat = 0, nedge = 0;
    for (uint32_t y = 0; y < DH; y++)
        for (uint32_t x = 0; x < DW; x++) {
            float fx = (x + 0.5f) * SW / DW - 0.5f, fy = (y + 0.5f) * SH / DH - 0.5f;
            int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
            float ax = fx - x0, ay = fy - y0;
            for (int ch = 0; ch < 4; ch++) {
                #define S(X, Y) (float)src[4 * ((((Y) < 0 ? 0 : (Y) >= SH ? SH - 1 : (Y)) * SW) + ((X) < 0 ? 0 : (X) >= SW ? SW - 1 : (X))) + ch]
                float v = (S(x0, y0) * (1 - ax) + S(x0 + 1, y0) * ax) * (1 - ay) + (S(x0, y0 + 1) * (1 - ax) + S(x0 + 1, y0 + 1) * ax) * ay;
                #undef S
                ref[4 * (y * DW + x) + ch] = (uint8_t)(v + 0.5f);
            }
            if (x == 0 || y == 0) continue;
            const uint8_t *g = p + 4 * (y * DW + x), *gl = g - 4, *gu = g - 4 * DW, *r = ref + 4 * (y * DW + x);
            float grad_lin = fabsf((float)r[1] - r[1 - 4]) + fabsf((float)r[1] - r[1 - 4 * (int)DW]);
            float grad_fsr = fabsf((float)g[1] - gl[1]) + fabsf((float)g[1] - gu[1]);
            if (grad_lin < 2) flat += fabs((double)g[1] - r[1]), nflat++;               // smooth parts: the same picture
            else if (grad_lin > 20) edge_fsr += grad_fsr, edge_lin += grad_lin, nedge++;   // edges: FSR's steeper
        }
    char path[1100];
    snprintf(path, sizeof path, "%s/upscale-source.ppm", dir); write_ppm(path, src, SW, SH);
    snprintf(path, sizeof path, "%s/upscale-fsr.ppm", dir); write_ppm(path, p, DW, DH);
    snprintf(path, sizeof path, "%s/upscale-bilinear.ppm", dir); write_ppm(path, ref, DW, DH);
    printf("upscale self-test: %ux%u -> %ux%u, flat difference %.2f over %ld pixels, edge steepness FSR %.1f bilinear %.1f over %ld pixels\n",
           SW, SH, DW, DH, nflat ? flat / nflat : -1, nflat, nedge ? edge_fsr / nedge : 0, nedge ? edge_lin / nedge : 0, nedge);
    fflush(stdout);
    free(ref);
    vkUnmapMemory(g_vk.dev, bm);
    vkDestroyBuffer(g_vk.dev, b, NULL); vkFreeMemory(g_vk.dev, bm, NULL);
    vkDestroyImageView(g_vk.dev, iv, NULL); vkDestroyImage(g_vk.dev, im, NULL); vkFreeMemory(g_vk.dev, imem, NULL);
}
