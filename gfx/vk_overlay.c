// SPDX-License-Identifier: GPL-3.0-or-later
// What NativeNX shows over the game: its pop-ups (achievements, at the top right as the PS3 showed its trophies) and
// its menu (the Home button / Shift+Tab: the game's achievements, a screenshot, back to NativeNX, closing the game).
// NativeNX makes the pictures and writes each to a file (NXLIMG01: straight alpha, NXLIMG02: premultiplied; width and
// height as 32-bit little-endian words, then RGBA rows) and sends lines down the pipe NXL_NOTIFY_FD names:
//   <path> or "pop <path>"   a pop-up: shown in turn, sliding in from the right, 5 seconds, sliding out
//   "menu <path>"            the menu, over the whole window, until "menu-off" (the game's input is held meanwhile)
//   "shot"                   a screenshot (as F12)
//   "hide", "show"           the window minimised (the game is paused by NativeNX then) and back
// Each is a quad with the picture, blended over the frame after the blit (a small pipeline of its own rendering into
// the swapchain image). Screenshots are of the game's frame and leave all this out. NXL_TEST_OVERLAY=<picture file>
// draws one over a blue image and checks the pixels (tests: t_window_overlay)
#define _GNU_SOURCE
#include "vk.h"
#include "platform.h"
#include <fcntl.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include "shaders/shaders_spv.h"

void vk_queue_lock(void);
void vk_queue_unlock(void);
void vk_idle(void);

typedef struct { uint8_t *px; uint32_t w, h; } pic_t;
static pic_t g_q[8];
static int g_nq, g_fd = -2;
static char g_line[2048];
static size_t g_line_n;
static double g_t0;            // when the pop-up shown now came on screen (0: not yet drawn)
// the pictures on the GPU: the pop-up shown now (0), the menu (1)
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkDescriptorSet ds; } layer_t;
static layer_t g_lay[2];
static pic_t g_menu;            // a new menu picture, to go to the GPU at the next frame
static _Atomic int g_menu_on;   // the menu is up (or asked for)
static _Atomic double g_menu_asked_at;   // when it was asked for: NativeNX not answering in 3 s gives the game its buttons back
static _Atomic int g_win_cmd;   // for the window's thread: 1 minimise, 2 restore
static VkDescriptorSetLayout g_dsl;
static VkDescriptorPool g_dpool;
static VkPipelineLayout g_pl;
static VkPipeline g_pipe;
static VkFormat g_pipe_fmt;
static VkSampler g_samp;
static VkCommandPool g_cpool;
typedef struct { VkImage img; VkImageView view; } ovl_view_t;
static ovl_view_t g_views[16];
static int g_nviews;

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static int srgb(VkFormat f) { return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB; }

// a picture file: read, premultiplied, the file removed (NativeNX made it for this)
static int load_pic(const char *path, pic_t *o) {
    FILE *f = fopen(path, "rb");
    if (!f) { VRED("pop-up: cannot open %s", path); return -1; }
    uint8_t h[16];
    int ok = fread(h, 1, 16, f) == 16 && (!memcmp(h, "NXLIMG01", 8) || !memcmp(h, "NXLIMG02", 8)), pre = ok && h[7] == '2';
    uint32_t w = ok ? (uint32_t)h[8] | (uint32_t)h[9] << 8 | (uint32_t)h[10] << 16 | (uint32_t)h[11] << 24 : 0;
    uint32_t hh = ok ? (uint32_t)h[12] | (uint32_t)h[13] << 8 | (uint32_t)h[14] << 16 | (uint32_t)h[15] << 24 : 0;
    if (!ok || !w || !hh || w > 4096 || hh > 4096) { fclose(f); VRED("pop-up: %s is not a picture NativeNX made", path); return -1; }
    uint8_t *px = malloc((size_t)w * hh * 4);
    if (fread(px, 4, (size_t)w * hh, f) != (size_t)w * hh) { fclose(f); free(px); return -1; }
    fclose(f);
    if (!getenv("NXL_TEST_OVERLAY")) unlink(path);
    if (!pre) for (size_t i = 0; i < (size_t)w * hh; i++) { uint32_t a = px[4 * i + 3]; for (int k = 0; k < 3; k++) px[4 * i + k] = (uint8_t)((px[4 * i + k] * a + 127) / 255); }
    *o = (pic_t){px, w, hh};
    return 0;
}
// the pipe: whole lines, each a picture's path
static void poll_pipe(void) {
    if (g_fd == -2) {
        g_fd = nxl_fd_from_env("NXL_NOTIFY_FD");   // (src/platform.h)
        if (g_fd >= 0) nxl_fd_nonblock(g_fd);
    }
    if (g_fd < 0) return;
    for (;;) {
        ssize_t k = read(g_fd, g_line + g_line_n, sizeof g_line - 1 - g_line_n);
        if (k == 0) { close(g_fd); g_fd = -1; return; }   // NativeNX is gone
        if (k < 0) return;                                   // nothing now
        g_line_n += (size_t)k;
        char *nl;
        while ((nl = memchr(g_line, '\n', g_line_n))) {
            *nl = 0;
            pic_t p;
            const char *l = g_line;
            if (!strncmp(l, "menu ", 5)) {   // the menu's picture (a newer one replaces one not shown yet)
                if (!load_pic(l + 5, &p)) { free(g_menu.px); g_menu = p; atomic_store(&g_menu_on, 1); }
            } else if (!strcmp(l, "menu-off")) {
                atomic_store(&g_menu_on, 0);
                free(g_menu.px), g_menu.px = NULL;
                void nxl_input_block(int);
                nxl_input_block(0);
                fprintf(stderr, "[vk] menu closed: back to the game\n");
            } else if (!strcmp(l, "shot")) { void vk_shot_request(void); vk_shot_request(); }
            else if (!strcmp(l, "hide")) atomic_store(&g_win_cmd, 1);
            else if (!strcmp(l, "show")) atomic_store(&g_win_cmd, 2);
            else {
                if (!strncmp(l, "pop ", 4)) l += 4;
                if (g_nq < 8 && !load_pic(l, &p)) { g_q[g_nq++] = p; fprintf(stderr, "[vk] pop-up: %ux%u\n", p.w, p.h); }
            }
            size_t used = (size_t)(nl - g_line) + 1;
            memmove(g_line, g_line + used, g_line_n - used);
            g_line_n -= used;
        }
        if (g_line_n == sizeof g_line - 1) g_line_n = 0;   // a line too long: dropped
    }
}

// ---------- Vulkan ----------
static void one_shot(void (*rec)(VkCommandBuffer, void *), void *arg) {
    if (!g_cpool) {
        VkCommandPoolCreateInfo ci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_vk.qfam};
        VK_CHECK(vkCreateCommandPool(g_vk.dev, &ci, NULL, &g_cpool));
    }
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, g_cpool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(g_vk.dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(cb, &bi);
    rec(cb, arg);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    vkCreateFence(g_vk.dev, &fi, NULL, &fence);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL};
    vk_queue_lock();
    VK_CHECK(vkQueueSubmit(g_vk.queue, 1, &si, fence));
    vk_queue_unlock();
    vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX);
    vkDestroyFence(g_vk.dev, fence, NULL);
    vkFreeCommandBuffers(g_vk.dev, g_cpool, 1, &cb);
}
// A host-visible buffer, mapped.
static int make_buffer(VkDeviceSize n, VkBufferUsageFlags use, VkBuffer *b, VkDeviceMemory *m, void **map) {
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, use, VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(g_vk.dev, &bci, NULL, b) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, *b, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    if (vkAllocateMemory(g_vk.dev, &mai, NULL, m) != VK_SUCCESS) { vkDestroyBuffer(g_vk.dev, *b, NULL); return -1; }
    vkBindBufferMemory(g_vk.dev, *b, *m, 0);
    return vkMapMemory(g_vk.dev, *m, 0, VK_WHOLE_SIZE, 0, map) == VK_SUCCESS ? 0 : -1;
}
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 s0, VkAccessFlags2 a0, VkPipelineStageFlags2 s1, VkAccessFlags2 a1) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, s0, a0, s1, a1, from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}
typedef struct { VkBuffer buf; uint32_t w, h; VkImage img; } upload_t;
// Records the upload of an overlay picture into its image.
static void rec_upload(VkCommandBuffer cb, void *arg) {
    upload_t *u = arg;
    barrier(cb, u->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {u->w, u->h, 1}};
    vkCmdCopyBufferToImage(cb, u->buf, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    barrier(cb, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
}
static void layout_make(void);
// Frees an overlay layer's image (after the GPU is idle).
static void pic_free_gpu(int li) {
    layer_t *L = &g_lay[li];
    if (!L->img) return;
    vk_idle();
    vkDestroyImageView(g_vk.dev, L->view, NULL);
    vkDestroyImage(g_vk.dev, L->img, NULL);
    vkFreeMemory(g_vk.dev, L->mem, NULL);
    L->img = VK_NULL_HANDLE, L->view = VK_NULL_HANDLE, L->mem = VK_NULL_HANDLE;
}
// a picture as a sampled image of layer li (sRGB when the window's format is: its values are as they look)
static int pic_upload(const pic_t *p, VkFormat target, int li) {
    layer_t *L = &g_lay[li];
    pic_free_gpu(li);
    VkFormat fmt = srgb(target) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, fmt, {p->w, p->h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_vk.dev, &ci, NULL, &L->img) != VK_SUCCESS) { L->img = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, L->img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &mai, NULL, &L->mem));
    vkBindImageMemory(g_vk.dev, L->img, L->mem, 0);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, L->img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VK_CHECK(vkCreateImageView(g_vk.dev, &vi, NULL, &L->view));
    VkBuffer sb;
    VkDeviceMemory sm;
    void *map;
    if (make_buffer((VkDeviceSize)p->w * p->h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &sb, &sm, &map)) return -1;
    memcpy(map, p->px, (size_t)p->w * p->h * 4);
    upload_t u = {sb, p->w, p->h, L->img};
    one_shot(rec_upload, &u);
    vkDestroyBuffer(g_vk.dev, sb, NULL);
    vkFreeMemory(g_vk.dev, sm, NULL);
    // the descriptor set: the picture and a linear sampler
    if (!g_samp) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                  VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        VK_CHECK(vkCreateSampler(g_vk.dev, &si, NULL, &g_samp));
    }
    layout_make();
    VkDescriptorImageInfo ii = {g_samp, L->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, L->ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ii};
    vkUpdateDescriptorSets(g_vk.dev, 1, &w, 0, NULL);
    return 0;
}
static VkShaderModule module(const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, bytes, code};
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(g_vk.dev, &ci, NULL, &m));
    return m;
}
static void layout_make(void) {   // the descriptor set (the picture) and the pipeline layout: once
    if (!g_dsl) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b};
        VK_CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &lci, NULL, &g_dsl));
        VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
        VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 2, 1, &ps};
        VK_CHECK(vkCreateDescriptorPool(g_vk.dev, &pci, NULL, &g_dpool));
        VkDescriptorSetLayout two[2] = {g_dsl, g_dsl};
        VkDescriptorSet sets[2];
        VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_dpool, 2, two};
        VK_CHECK(vkAllocateDescriptorSets(g_vk.dev, &ai, sets));
        g_lay[0].ds = sets[0], g_lay[1].ds = sets[1];
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 20};
        VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &g_dsl, 1, &pr};
        VK_CHECK(vkCreatePipelineLayout(g_vk.dev, &plci, NULL, &g_pl));
    }
}
// The pipeline that draws the overlay's pictures over the frame, for the swapchain's format (made again when it
// changes).
static int pipe_make(VkFormat fmt) {
    if (g_pipe && g_pipe_fmt == fmt) return 0;
    layout_make();
    if (g_pipe) { vk_idle(); vkDestroyPipeline(g_vk.dev, g_pipe, NULL); g_pipe = VK_NULL_HANDLE; }
    VkShaderModule vs = module(spv_overlay_vert, sizeof spv_overlay_vert), fs = module(spv_overlay_frag, sizeof spv_overlay_frag);
    VkPipelineShaderStageCreateInfo st[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
                                             {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkPipelineVertexInputStateCreateInfo vin = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_FALSE};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE,
                                                 VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState ba = {VK_TRUE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_OP_ADD,
                                              VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &fmt};
    VkGraphicsPipelineCreateInfo gci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, 2, st, &vin, &ia, NULL, &vp, &rs, &ms, NULL, &cb, &ds, g_pl};
    VkResult r = vkCreateGraphicsPipelines(g_vk.dev, VK_NULL_HANDLE, 1, &gci, NULL, &g_pipe);
    vkDestroyShaderModule(g_vk.dev, vs, NULL), vkDestroyShaderModule(g_vk.dev, fs, NULL);
    if (r != VK_SUCCESS) { VRED("pop-ups: the pipeline could not be made (%d)", r); g_pipe = VK_NULL_HANDLE; return -1; }
    g_pipe_fmt = fmt;
    return 0;
}
// An image view of a swapchain image (made once per image, up to 16).
static VkImageView view_of(VkImage img, VkFormat fmt) {
    for (int i = 0; i < g_nviews; i++) if (g_views[i].img == img) return g_views[i].view;
    if (g_nviews == 16) return VK_NULL_HANDLE;
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, {0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g_vk.dev, &vi, NULL, &v) != VK_SUCCESS) return VK_NULL_HANDLE;
    g_views[g_nviews++] = (ovl_view_t){img, v};
    return v;
}
// the swapchain's images go (resize): their views with them (the device is idle then)
void vk_overlay_swapchain_gone(void) {
    for (int i = 0; i < g_nviews; i++) vkDestroyImageView(g_vk.dev, g_views[i].view, NULL);
    g_nviews = 0;
}
// how far the pop-up shown now is: 0 off screen at the right, 1 in place; < 0 done
static float slide(double t) {
    const double in = 0.35, stay = 5.0;
    if (t < in) { double u = 1 - t / in; return (float)(1 - u * u * u); }
    if (t < in + stay) return 1;
    if (t < 2 * in + stay) { double u = (t - in - stay) / in; return (float)(1 - u * u); }
    return -1;
}
// over the frame in img (TRANSFER_DST_OPTIMAL, left so): layer li's picture in the rectangle (pixels), faded by alpha
static void draw_quad(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t w, uint32_t h, int li, float x0, float y0, float pw, float ph, float alpha) {
    VkImageView v = view_of(img, fmt);
    if (!v || pipe_make(fmt) || !g_lay[li].img) return;
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE,
                                    VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, 1, &ca};
    vkCmdBeginRendering(cb, &ri);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pl, 0, 1, &g_lay[li].ds, 0, NULL);
    VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc = {{0, 0}, {w, h}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    float pc[5] = {x0 / w * 2 - 1, y0 / h * 2 - 1, (x0 + pw) / w * 2 - 1, (y0 + ph) / h * 2 - 1, alpha};
    vkCmdPushConstants(cb, g_pl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, pc);
    vkCmdDraw(cb, 4, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);
}
// the pop-up shown now, t seconds after it came: 9% of the window's height (the picture is 880 x 184), 2% from the top
// and the right, sliding
static void draw_at(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t w, uint32_t h, double t) {
    float s = slide(t);
    if (s < 0) return;
    float ph = h * 0.09f, pw = ph * g_q[0].w / g_q[0].h, m = h * 0.02f;
    if (pw > w * 0.9f) pw = w * 0.9f, ph = pw * g_q[0].h / g_q[0].w;
    draw_quad(cb, img, fmt, w, h, 0, w - m - pw + (1 - s) * (pw + m + 4), m, pw, ph, fminf(1, s * 1.2f));
}
// the window's thread: what NativeNX asked of the window (1 minimise, 2 restore, 0 nothing), once
int vk_overlay_window_cmd(void) { return atomic_exchange(&g_win_cmd, 0); }
// the menu: up (or asked for) / asked for by the window's thread (the Home button)
int vk_menu_on(void) { return atomic_load(&g_menu_on); }
void vk_menu_asked(void) { g_menu_asked_at = now_s(); atomic_store(&g_menu_on, 1); }
// each frame shown in the window (vk_present.c), after the game's frame is in img
void vk_overlay_draw(VkCommandBuffer cb, VkImage img, VkFormat fmt, uint32_t w, uint32_t h) {
    poll_pipe();
    if (g_menu.px) { pic_upload(&g_menu, fmt, 1); free(g_menu.px); g_menu.px = NULL; g_menu_asked_at = 0; }   // a new menu picture
    if (atomic_load(&g_menu_on) && !g_lay[1].img && g_menu_asked_at && now_s() - g_menu_asked_at > 3) {
        VRED("the menu: NativeNX did not answer the Home button: back to the game");
        atomic_store(&g_menu_on, 0), g_menu_asked_at = 0;
        void nxl_input_block(int);
        nxl_input_block(0);
    }
    if (atomic_load(&g_menu_on) && g_lay[1].img) draw_quad(cb, img, fmt, w, h, 1, 0, 0, (float)w, (float)h, 1);
    else if (!atomic_load(&g_menu_on) && g_lay[1].img) pic_free_gpu(1);
    if (!g_nq) return;
    double now = now_s();
    if (!g_lay[0].img || g_t0 == 0) {   // the next pop-up: its picture to the GPU, its time from now
        if (pic_upload(&g_q[0], fmt, 0)) { free(g_q[0].px); memmove(g_q, g_q + 1, (size_t)(--g_nq) * sizeof *g_q); return; }
        g_t0 = now;
    }
    if (slide(now - g_t0) < 0) {   // done: the next one, at the next frame
        free(g_q[0].px);
        memmove(g_q, g_q + 1, (size_t)(--g_nq) * sizeof *g_q);
        g_t0 = 0;
        return;
    }
    draw_at(cb, img, fmt, w, h, now - g_t0);
}

// ---------- NXL_TEST_OVERLAY: one pop-up over a blue image, the pixels checked ----------
typedef struct { VkImage img; VkBuffer buf; uint32_t w, h; } st_t;
// Records the self-test: a blue frame with the overlay drawn over it.
static void rec_test(VkCommandBuffer cb, void *arg) {
    st_t *s = arg;
    barrier(cb, s->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkClearColorValue blue = {{0, 0, 1, 1}};
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &blue, 1, &rr);
    barrier(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    draw_at(cb, s->img, VK_FORMAT_R8G8B8A8_UNORM, s->w, s->h, 2.0);   // in place
    barrier(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {s->w, s->h, 1}};
    vkCmdCopyImageToBuffer(cb, s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->buf, 1, &r);
}
// Draws a test picture over a blue frame and writes the result, for the tests.
void vk_overlay_selftest(const char *path) {
    pic_t p;
    if (load_pic(path, &p)) { printf("not ok - the test picture %s\n", path); return; }
    g_q[0] = p, g_nq = 1;
    st_t s = {VK_NULL_HANDLE, VK_NULL_HANDLE, 1000, 500};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, {s.w, s.h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VK_CHECK(vkCreateImage(g_vk.dev, &ci, NULL, &s.img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, s.img, &mr);
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory im;
    VK_CHECK(vkAllocateMemory(g_vk.dev, &mai, NULL, &im));
    vkBindImageMemory(g_vk.dev, s.img, im, 0);
    VkDeviceMemory bm;
    uint8_t *out;
    if (make_buffer((VkDeviceSize)s.w * s.h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s.buf, &bm, (void **)&out)) { printf("not ok - a buffer to read the test image back\n"); return; }
    if (pic_upload(&p, VK_FORMAT_R8G8B8A8_UNORM, 0)) { printf("not ok - the picture to the GPU\n"); return; }
    one_shot(rec_test, &s);
    // the test picture is one opaque colour; the pop-up: 9% of 500 = 45 high, 215 wide, 10 from the top and the right
    const uint8_t *in_ = out + 4 * ((size_t)30 * s.w + (s.w - 10 - 100)), *mid = out + 4 * ((size_t)250 * s.w + 500), *left = out + 4 * ((size_t)30 * s.w + (s.w - 10 - 230));
    const uint8_t *c = p.px + 4 * ((size_t)(p.h / 2) * p.w + p.w / 2);
    int ok_in = abs(in_[0] - c[0]) <= 3 && abs(in_[1] - c[1]) <= 3 && abs(in_[2] - c[2]) <= 3;
    printf("%s - the pop-up at the top right, over the frame (%u %u %u, the picture's %u %u %u)\n", ok_in ? "ok" : "not ok", in_[0], in_[1], in_[2], c[0], c[1], c[2]);
    printf("%s - the rest of the frame untouched (%u %u %u)\n", mid[0] == 0 && mid[1] == 0 && mid[2] == 255 ? "ok" : "not ok", mid[0], mid[1], mid[2]);
    printf("%s - beside it, the frame (%u %u %u)\n", left[0] == 0 && left[1] == 0 && left[2] == 255 ? "ok" : "not ok", left[0], left[1], left[2]);
    printf("nxl-overlay-test: done\n");
    fflush(stdout);
}
