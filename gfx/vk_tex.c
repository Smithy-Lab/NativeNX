// SPDX-License-Identifier: GPL-3.0-or-later
// Formats, textures and samplers of the Vulkan backend (see vk.h).
//
// An NVN texture's storage is the game's memory, in the Tegra block-linear layout. Here each texture object gets a
// Vulkan image: its contents come from the game's memory (deswizzled on upload, nxnative's nvn_deswizzle.c; ASTC is
// decoded to RGBA8 when the GPU has no ASTC) until the GPU writes it (a render target, a storage image, a copy's
// destination), after which the image holds the contents. Texture objects over the same storage with the same shape
// share one image (games re-initialise texture objects over their render targets), so what one writes the other sees.
// CPU-written storage is uploaded again when its pages were written since the last upload (nxnative's nvn_dirty.c
// write-protect tracking).
#define _GNU_SOURCE
#include "vk.h"
#include "nvn_tex.h"
#include "nvn_dirty.h"
#include "tex/nvn_deswizzle.h"
#include <sys/stat.h>
#include <math.h>
#include "nxl_shader.h"

void nxl_astc_decode(const uint8_t *data, size_t n, uint32_t w, uint32_t h, uint32_t d, uint32_t bw, uint32_t bh, uint8_t *out);

// ---------- formats ----------
#define C(r, g, b, a) {VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a}
#define ID C(R, G, B, A)
#define NOA C(R, G, B, ONE)
#define F(vkf, bpb, ...) {VK_FORMAT_##vkf, bpb, 1, 1, __VA_ARGS__}
// vk, bpb, bw, bh, depth, stencil, integer, signed, srgb, decode, swizzle
static vk_fmt T[0x9D] = {
    [0x01] = F(R8_UNORM, 1, 0, 0, 0, 0, 0, 0, ID), F(R8_SNORM, 1, 0, 0, 0, 0, 0, 0, ID), F(R8_UINT, 1, 0, 0, 1, 0, 0, 0, ID), F(R8_SINT, 1, 0, 0, 1, 1, 0, 0, ID),
    [0x05] = F(R16_SFLOAT, 2, 0, 0, 0, 0, 0, 0, ID), F(R16_UNORM, 2, 0, 0, 0, 0, 0, 0, ID), F(R16_SNORM, 2, 0, 0, 0, 0, 0, 0, ID), F(R16_UINT, 2, 0, 0, 1, 0, 0, 0, ID), F(R16_SINT, 2, 0, 0, 1, 1, 0, 0, ID),
    [0x0A] = F(R32_SFLOAT, 4, 0, 0, 0, 0, 0, 0, ID), F(R32_UINT, 4, 0, 0, 1, 0, 0, 0, ID), F(R32_SINT, 4, 0, 0, 1, 1, 0, 0, ID),
    [0x0D] = F(R8G8_UNORM, 2, 0, 0, 0, 0, 0, 0, ID), F(R8G8_SNORM, 2, 0, 0, 0, 0, 0, 0, ID), F(R8G8_UINT, 2, 0, 0, 1, 0, 0, 0, ID), F(R8G8_SINT, 2, 0, 0, 1, 1, 0, 0, ID),
    [0x11] = F(R16G16_SFLOAT, 4, 0, 0, 0, 0, 0, 0, ID), F(R16G16_UNORM, 4, 0, 0, 0, 0, 0, 0, ID), F(R16G16_SNORM, 4, 0, 0, 0, 0, 0, 0, ID), F(R16G16_UINT, 4, 0, 0, 1, 0, 0, 0, ID), F(R16G16_SINT, 4, 0, 0, 1, 1, 0, 0, ID),
    [0x16] = F(R32G32_SFLOAT, 8, 0, 0, 0, 0, 0, 0, ID), F(R32G32_UINT, 8, 0, 0, 1, 0, 0, 0, ID), F(R32G32_SINT, 8, 0, 0, 1, 1, 0, 0, ID),
    [0x19] = F(R8G8B8_UNORM, 3, 0, 0, 0, 0, 0, 0, ID), F(R8G8B8_SNORM, 3, 0, 0, 0, 0, 0, 0, ID), F(R8G8B8_UINT, 3, 0, 0, 1, 0, 0, 0, ID), F(R8G8B8_SINT, 3, 0, 0, 1, 1, 0, 0, ID),
    [0x1D] = F(R16G16B16_SFLOAT, 6, 0, 0, 0, 0, 0, 0, ID), F(R16G16B16_UNORM, 6, 0, 0, 0, 0, 0, 0, ID), F(R16G16B16_SNORM, 6, 0, 0, 0, 0, 0, 0, ID),
    F(R16G16B16_UINT, 6, 0, 0, 1, 0, 0, 0, ID), F(R16G16B16_SINT, 6, 0, 0, 1, 1, 0, 0, ID),
    [0x22] = F(R32G32B32_SFLOAT, 12, 0, 0, 0, 0, 0, 0, ID), F(R32G32B32_UINT, 12, 0, 0, 1, 0, 0, 0, ID), F(R32G32B32_SINT, 12, 0, 0, 1, 1, 0, 0, ID),
    [0x25] = F(R8G8B8A8_UNORM, 4, 0, 0, 0, 0, 0, 0, ID), F(R8G8B8A8_SNORM, 4, 0, 0, 0, 0, 0, 0, ID), F(R8G8B8A8_UINT, 4, 0, 0, 1, 0, 0, 0, ID), F(R8G8B8A8_SINT, 4, 0, 0, 1, 1, 0, 0, ID),
    [0x29] = F(R16G16B16A16_SFLOAT, 8, 0, 0, 0, 0, 0, 0, ID), F(R16G16B16A16_UNORM, 8, 0, 0, 0, 0, 0, 0, ID), F(R16G16B16A16_SNORM, 8, 0, 0, 0, 0, 0, 0, ID),
    F(R16G16B16A16_UINT, 8, 0, 0, 1, 0, 0, 0, ID), F(R16G16B16A16_SINT, 8, 0, 0, 1, 1, 0, 0, ID),
    [0x2E] = F(R32G32B32A32_SFLOAT, 16, 0, 0, 0, 0, 0, 0, ID), F(R32G32B32A32_UINT, 16, 0, 0, 1, 0, 0, 0, ID), F(R32G32B32A32_SINT, 16, 0, 0, 1, 1, 0, 0, ID),
    [0x31] = F(S8_UINT, 1, 0, 1, 1, 0, 0, 0, ID), F(D16_UNORM, 2, 1, 0, 0, 0, 0, 0, ID),
    [0x33] = F(X8_D24_UNORM_PACK32, 4, 1, 0, 0, 0, 0, 0, ID), F(D32_SFLOAT, 4, 1, 0, 0, 0, 0, 0, ID),
    [0x35] = F(D24_UNORM_S8_UINT, 4, 1, 1, 0, 0, 0, 0, ID), F(D32_SFLOAT_S8_UINT, 8, 1, 1, 0, 0, 0, 0, ID),
    [0x37] = F(R8G8B8A8_SRGB, 4, 0, 0, 0, 0, 1, 0, NOA), F(R8G8B8A8_SRGB, 4, 0, 0, 0, 0, 1, 0, ID),
    [0x39] = F(A4B4G4R4_UNORM_PACK16, 2, 0, 0, 0, 0, 0, 0, ID), F(A1B5G5R5_UNORM_PACK16_KHR, 2, 0, 0, 0, 0, 0, 0, NOA),
    F(A1B5G5R5_UNORM_PACK16_KHR, 2, 0, 0, 0, 0, 0, 0, ID), F(R5G6B5_UNORM_PACK16, 2, 0, 0, 0, 0, 0, 0, C(B, G, R, A)),
    [0x3D] = F(A2B10G10R10_UNORM_PACK32, 4, 0, 0, 0, 0, 0, 0, ID), F(A2B10G10R10_UINT_PACK32, 4, 0, 0, 1, 0, 0, 0, ID),
    F(B10G11R11_UFLOAT_PACK32, 4, 0, 0, 0, 0, 0, 0, ID), F(E5B9G9R9_UFLOAT_PACK32, 4, 0, 0, 0, 0, 0, 0, ID),
    [0x41] = {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8, 4, 4, 0, 0, 0, 0, 0, 0, NOA}, {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 4, 0, 0, 0, 0, 0, 0, ID},
    {VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID}, {VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID},
    {VK_FORMAT_BC1_RGB_SRGB_BLOCK, 8, 4, 4, 0, 0, 0, 0, 1, 0, NOA}, {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8, 4, 4, 0, 0, 0, 0, 1, 0, ID},
    {VK_FORMAT_BC2_SRGB_BLOCK, 16, 4, 4, 0, 0, 0, 0, 1, 0, ID}, {VK_FORMAT_BC3_SRGB_BLOCK, 16, 4, 4, 0, 0, 0, 0, 1, 0, ID},
    [0x49] = {VK_FORMAT_BC4_UNORM_BLOCK, 8, 4, 4, 0, 0, 0, 0, 0, 0, ID}, {VK_FORMAT_BC4_SNORM_BLOCK, 8, 4, 4, 0, 0, 0, 0, 0, 0, ID},
    {VK_FORMAT_BC5_UNORM_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID}, {VK_FORMAT_BC5_SNORM_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID},
    {VK_FORMAT_BC7_UNORM_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID}, {VK_FORMAT_BC7_SRGB_BLOCK, 16, 4, 4, 0, 0, 0, 0, 1, 0, ID},
    {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID}, {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16, 4, 4, 0, 0, 0, 0, 0, 0, ID},
    // UI2F / I2F: integer data read as floats (as vertex formats: VK *_USCALED / *_SSCALED; as textures, the integer formats)
    [0x51] = F(R8_UINT, 1, 0, 0, 1, 0, 0, 0, ID), F(R8_SINT, 1, 0, 0, 1, 1, 0, 0, ID), F(R16_UINT, 2, 0, 0, 1, 0, 0, 0, ID), F(R16_SINT, 2, 0, 0, 1, 1, 0, 0, ID),
    F(R32_UINT, 4, 0, 0, 1, 0, 0, 0, ID), F(R32_SINT, 4, 0, 0, 1, 1, 0, 0, ID),
    [0x57] = F(R8G8_UINT, 2, 0, 0, 1, 0, 0, 0, ID), F(R8G8_SINT, 2, 0, 0, 1, 1, 0, 0, ID), F(R16G16_UINT, 4, 0, 0, 1, 0, 0, 0, ID), F(R16G16_SINT, 4, 0, 0, 1, 1, 0, 0, ID),
    F(R32G32_UINT, 8, 0, 0, 1, 0, 0, 0, ID), F(R32G32_SINT, 8, 0, 0, 1, 1, 0, 0, ID),
    [0x5D] = F(R8G8B8_UINT, 3, 0, 0, 1, 0, 0, 0, ID), F(R8G8B8_SINT, 3, 0, 0, 1, 1, 0, 0, ID), F(R16G16B16_UINT, 6, 0, 0, 1, 0, 0, 0, ID), F(R16G16B16_SINT, 6, 0, 0, 1, 1, 0, 0, ID),
    F(R32G32B32_UINT, 12, 0, 0, 1, 0, 0, 0, ID), F(R32G32B32_SINT, 12, 0, 0, 1, 1, 0, 0, ID),
    [0x63] = F(R8G8B8A8_UINT, 4, 0, 0, 1, 0, 0, 0, ID), F(R8G8B8A8_SINT, 4, 0, 0, 1, 1, 0, 0, ID), F(R16G16B16A16_UINT, 8, 0, 0, 1, 0, 0, 0, ID),
    F(R16G16B16A16_SINT, 8, 0, 0, 1, 1, 0, 0, ID), F(R32G32B32A32_UINT, 16, 0, 0, 1, 0, 0, 0, ID), F(R32G32B32A32_SINT, 16, 0, 0, 1, 1, 0, 0, ID),
    [0x69] = F(A2B10G10R10_SNORM_PACK32, 4, 0, 0, 0, 0, 0, 0, ID), F(A2B10G10R10_SINT_PACK32, 4, 0, 0, 1, 1, 0, 0, ID),
    F(A2B10G10R10_UINT_PACK32, 4, 0, 0, 1, 0, 0, 0, ID), F(A2B10G10R10_SINT_PACK32, 4, 0, 0, 1, 1, 0, 0, ID),
    [0x6D] = F(R8G8B8A8_UNORM, 4, 0, 0, 0, 0, 0, 0, NOA), F(R8G8B8A8_SNORM, 4, 0, 0, 0, 0, 0, 0, NOA), F(R8G8B8A8_UINT, 4, 0, 0, 1, 0, 0, 0, NOA), F(R8G8B8A8_SINT, 4, 0, 0, 1, 1, 0, 0, NOA),
    [0x71] = F(R16G16B16A16_SFLOAT, 8, 0, 0, 0, 0, 0, 0, NOA), F(R16G16B16A16_UNORM, 8, 0, 0, 0, 0, 0, 0, NOA), F(R16G16B16A16_SNORM, 8, 0, 0, 0, 0, 0, 0, NOA),
    F(R16G16B16A16_UINT, 8, 0, 0, 1, 0, 0, 0, NOA), F(R16G16B16A16_SINT, 8, 0, 0, 1, 1, 0, 0, NOA),
    [0x76] = F(R32G32B32A32_SFLOAT, 16, 0, 0, 0, 0, 0, 0, NOA), F(R32G32B32A32_UINT, 16, 0, 0, 1, 0, 0, 0, NOA), F(R32G32B32A32_SINT, 16, 0, 0, 1, 1, 0, 0, NOA),
    // the BGR family is laid out as the Vulkan formats of the reversed names (as MoltenVK pairs them with Metal's: BGR565
    // = R5G6B5, BGR5A1 = A1R5G5B5, A1BGR5 = R5G5B5A1), so it reads with no channel mapping; it had red and blue swapped
    [0x95] = F(R5G6B5_UNORM_PACK16, 2, 0, 0, 0, 0, 0, 0, ID), F(A1R5G5B5_UNORM_PACK16, 2, 0, 0, 0, 0, 0, 0, NOA),
    F(A1R5G5B5_UNORM_PACK16, 2, 0, 0, 0, 0, 0, 0, ID), F(R5G5B5A1_UNORM_PACK16, 2, 0, 0, 0, 0, 0, 0, ID),
    [0x99] = F(B8G8R8A8_UNORM, 4, 0, 0, 0, 0, 0, 0, NOA), F(B8G8R8A8_UNORM, 4, 0, 0, 0, 0, 0, 0, ID), F(B8G8R8A8_SRGB, 4, 0, 0, 0, 0, 1, 0, NOA), F(B8G8R8A8_SRGB, 4, 0, 0, 0, 0, 1, 0, ID),
};
static const uint8_t astc_dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static const VkFormat astc_vk[14] = {VK_FORMAT_ASTC_4x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x4_UNORM_BLOCK, VK_FORMAT_ASTC_5x5_UNORM_BLOCK, VK_FORMAT_ASTC_6x5_UNORM_BLOCK,
                                     VK_FORMAT_ASTC_6x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x5_UNORM_BLOCK, VK_FORMAT_ASTC_8x6_UNORM_BLOCK, VK_FORMAT_ASTC_8x8_UNORM_BLOCK,
                                     VK_FORMAT_ASTC_10x5_UNORM_BLOCK, VK_FORMAT_ASTC_10x6_UNORM_BLOCK, VK_FORMAT_ASTC_10x8_UNORM_BLOCK, VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
                                     VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK};
static vk_fmt g_astc[28];
static int g_fmt_init;
static VkFormatFeatureFlags fmt_features(VkFormat f);
// Fills the ASTC entries of the format table: native ASTC where the GPU has it, else RGBA8 decoded on the CPU.
static void fmt_init(void) {
    for (int i = 0; i < 28; i++) {
        int k = i % 14, srgb = i >= 14;
        vk_fmt f = {astc_vk[k], 16, astc_dims[k][0], astc_dims[k][1], 0, 0, 0, 0, (uint8_t)srgb, 0, ID};
        if (srgb) f.vk = (VkFormat)(f.vk + 1);   // each ASTC SRGB format follows its UNORM one
        if (!g_vk.has_astc) f.vk = srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, f.decode = 1;
        g_astc[i] = f;
    }
    // RGB565 has red in the low bits: B5G6R5 in Vulkan's naming. R5G6B5 with red and blue swapped by the mapping reads
    // uploaded texels right but not what the GPU drew into it (the mapping does not apply to writes)
    // RGB5 / RGB5A1 / RGBA4 have red in the low bits too: A1B5G5R5 (VK_KHR_maintenance5) and A4B4G4R4 (Vulkan 1.3). Where
    // the device lacks the first, A1R5G5B5 with red and blue swapped reads uploads right
    if (!(fmt_features(VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        for (int i = 0x3A; i <= 0x3B; i++) {
            T[i].vk = VK_FORMAT_A1R5G5B5_UNORM_PACK16;
            VkComponentSwizzle sw[4] = {VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, i == 0x3A ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_A};
            memcpy(T[i].swz, sw, sizeof sw);
        }
    VkFormatFeatureFlags f565 = fmt_features(VK_FORMAT_B5G6R5_UNORM_PACK16);
    if ((f565 & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) && (f565 & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) T[0x3C].vk = VK_FORMAT_B5G6R5_UNORM_PACK16, memcpy(T[0x3C].swz, (VkComponentSwizzle[4])ID, sizeof T[0x3C].swz);
    if (!g_vk.has_d24s8) T[0x35].vk = VK_FORMAT_D32_SFLOAT_S8_UINT, T[0x35].bpb = 8;   // keeps the guest's bpb for uploads? no: depth is never uploaded
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, VK_FORMAT_X8_D24_UNORM_PACK32, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) T[0x33].vk = VK_FORMAT_D32_SFLOAT;
    g_fmt_init = 1;
}
const vk_fmt *vk_format(int f) {
    if (!g_fmt_init) fmt_init();
    if (f >= 0x79 && f <= 0x94) return &g_astc[f - 0x79];
    if (f > 0 && f < (int)(sizeof T / sizeof *T) && T[f].vk) return &T[f];
    return NULL;
}
int vk_fmt_yuzu_pixel(int f) {   // a few common ones (typeless image loads); the rest read as RGBA8
    switch (f) {
    case 0x25: return 0;  case 0x26: return 1;  case 0x28: return 2;  case 0x27: return 3;
    case 0x01: return 12; case 0x29: return 16; case 0x3F: return 21; case 0x2F: return 22;
    case 0x2E: return 35; case 0x30: return 36; case 0x16: return 37; case 0x0A: return 39;
    case 0x05: return 40; case 0x11: return 46; case 0x0B: return 58; case 0x0C: return 59;
    default: return -1;
    }
}

// The Vulkan format of an NVN vertex attribute format.
VkFormat vk_vertex_format(int f) {
    switch (f) {
    case 0x51: return VK_FORMAT_R8_USCALED; case 0x52: return VK_FORMAT_R8_SSCALED; case 0x53: return VK_FORMAT_R16_USCALED;
    case 0x54: return VK_FORMAT_R16_SSCALED; case 0x55: return VK_FORMAT_R32_UINT; case 0x56: return VK_FORMAT_R32_SINT;
    case 0x57: return VK_FORMAT_R8G8_USCALED; case 0x58: return VK_FORMAT_R8G8_SSCALED; case 0x59: return VK_FORMAT_R16G16_USCALED;
    case 0x5A: return VK_FORMAT_R16G16_SSCALED; case 0x5B: return VK_FORMAT_R32G32_UINT; case 0x5C: return VK_FORMAT_R32G32_SINT;
    case 0x5D: return VK_FORMAT_R8G8B8_USCALED; case 0x5E: return VK_FORMAT_R8G8B8_SSCALED; case 0x5F: return VK_FORMAT_R16G16B16_USCALED;
    case 0x60: return VK_FORMAT_R16G16B16_SSCALED; case 0x61: return VK_FORMAT_R32G32B32_UINT; case 0x62: return VK_FORMAT_R32G32B32_SINT;
    case 0x63: return VK_FORMAT_R8G8B8A8_USCALED; case 0x64: return VK_FORMAT_R8G8B8A8_SSCALED; case 0x65: return VK_FORMAT_R16G16B16A16_USCALED;
    case 0x66: return VK_FORMAT_R16G16B16A16_SSCALED; case 0x67: return VK_FORMAT_R32G32B32A32_UINT; case 0x68: return VK_FORMAT_R32G32B32A32_SINT;
    case 0x6B: return VK_FORMAT_A2B10G10R10_USCALED_PACK32; case 0x6C: return VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    case 0x6D: return VK_FORMAT_R8G8B8A8_UNORM; case 0x9A: case 0x99: return VK_FORMAT_B8G8R8A8_UNORM;
    default: { const vk_fmt *v = vk_format(f); return v && !v->depth && !v->stencil && v->bw == 1 ? v->vk : VK_FORMAT_UNDEFINED; }
    }
}

// What the GPU can do with a format in optimal tiling (asked once per format, then remembered).
static VkFormatFeatureFlags fmt_features(VkFormat f) {
    static VkFormat cache_f[64]; static VkFormatFeatureFlags cache_v[64]; static int n;
    for (int i = 0; i < n; i++) if (cache_f[i] == f) return cache_v[i];
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_vk.phys, f, &fp);
    if (n < 64) cache_f[n] = f, cache_v[n++] = fp.optimalTilingFeatures;
    return fp.optimalTilingFeatures;
}

// ---------- a small map: 64-bit key to pointer ----------
typedef struct { uint64_t *k; void **v; uint32_t cap, n; } u64map;
// A uint64 to pointer hash table (open addressing): the value of a key, or NULL.
static void *map_get(u64map *m, uint64_t k) {
    if (!m->cap) return NULL;
    for (uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 40) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == k) return m->v[i];
        if (!m->k[i]) return NULL;
    }
}
static void map_put(u64map *m, uint64_t k, void *v);
// Doubles the hash table and puts its entries back.
static void map_grow(u64map *m) {
    u64map o = *m;
    m->cap = o.cap ? o.cap * 2 : 1024, m->n = 0;
    m->k = calloc(m->cap, 8), m->v = calloc(m->cap, sizeof(void *));
    for (uint32_t i = 0; i < o.cap; i++) if (o.k[i] && o.v[i]) map_put(m, o.k[i], o.v[i]);
    free(o.k), free(o.v);
}
// Sets a key's value in the hash table (a NULL value leaves an entry that lookups pass over).
static void map_put(u64map *m, uint64_t k, void *v) {
    if ((m->n + 1) * 2 > m->cap) map_grow(m);
    for (uint32_t i = (uint32_t)(k * 0x9E3779B97F4A7C15ull >> 40) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        if (m->k[i] == k) { m->v[i] = v; return; }
        if (!m->k[i]) { m->k[i] = k, m->v[i] = v, m->n++; return; }
    }
}

// ---------- images (shared by texture objects over the same storage) ----------
typedef struct { uint64_t key; VkImageView v; uint32_t layers; } view_ent;
static __thread uint32_t g_view_layers;   // the layer count of the view vk_tex_view returned last (render targets: the pass's layers)
uint32_t vk_tex_view_layers(void) { return g_view_layers; }
typedef struct vk_img {
    VkImage im;
    VkDeviceMemory m;
    VkFormat fmt;
    VkImageAspectFlags aspect;
    uint32_t w, h, d, layers, levels, samples;   // the texture's size (as the game sees it)
    uint32_t hw, hh;            // the Vulkan image's size: w x h, or larger when the resolution scale applies (sc)
    uint8_t sc;                 // drawn at the resolution scale (vk_tex_rescale)
    uint8_t noscale;            // used in a way the scale cannot follow (a compute shader writes it, a copy from memory): kept at 1x
    VkImageUsageFlags usage; VkImageCreateFlags cflags; VkImageType itype;
    int target;                 // NVNtextureTarget it was made for
    int gpu;                    // the GPU wrote it: never uploaded again
    int uploaded;               // its contents came from guest memory at least once
    uint64_t stamp;             // nvn_dirty stamp of the last upload
    int refs;
    uint64_t alias_key;
    view_ent views[24];
    int nviews;
    VkBufferView bview;         // a texture buffer
    VkBufferView bview_raw[4]; VkFormat bview_raw_fmt[4];   // the same, as typed (raw) storage declarations read it
} vk_img;
struct vk_tex { tex_t t; vk_img *img; };

static u64map g_by_serial;      // serial → vk_tex
static u64map g_by_alias;       // storage + shape → vk_img
static pthread_mutex_t g_tex_mu = PTHREAD_MUTEX_INITIALIZER;

// The Vulkan image and view type, size and layers of an NVN texture's target (1D, 2D, 3D, arrays, cube maps).
static void tex_geom(const nvn_texdesc *d, VkImageType *type, VkImageViewType *vt, uint32_t *w, uint32_t *h, uint32_t *z, uint32_t *layers, VkImageCreateFlags *fl) {
    *w = d->width ? d->width : 1, *h = d->height ? d->height : 1, *z = 1, *layers = 1, *fl = 0;
    switch (d->target) {
    case 0: *type = VK_IMAGE_TYPE_1D, *vt = VK_IMAGE_VIEW_TYPE_1D, *h = 1; break;
    case 3: *type = VK_IMAGE_TYPE_1D, *vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY, *layers = *h, *h = 1; break;
    case 2: *type = VK_IMAGE_TYPE_3D, *vt = VK_IMAGE_VIEW_TYPE_3D, *z = d->depth ? d->depth : 1, *fl = VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT; break;
    case 4: case 6: *type = VK_IMAGE_TYPE_2D, *vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY, *layers = d->depth ? d->depth : 1; break;
    case 8: *type = VK_IMAGE_TYPE_2D, *vt = VK_IMAGE_VIEW_TYPE_CUBE, *layers = 6, *fl = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    case 9: *type = VK_IMAGE_TYPE_2D, *vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, *layers = d->depth >= 6 ? d->depth / 6 * 6 : 6, *fl = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT; break;
    default: *type = VK_IMAGE_TYPE_2D, *vt = VK_IMAGE_VIEW_TYPE_2D; break;
    }
    if (*type == VK_IMAGE_TYPE_2D && *w == *h && !(*fl & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) && *layers >= 6 && d->samples <= 1) *fl |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
}

// where texture work is recorded: the submission's command buffer when the submitting thread asks (after closing an
// open render pass: transfers cannot run inside one), else one-off work of its own
void vk_draw_suspend(void);
static VkCommandBuffer tex_cmd(int *once) {
    VkCommandBuffer cb = vk_cmd();
    if (cb) { vk_draw_suspend(); *once = 0; return cb; }
    *once = 1;
    return vk_once_begin();
}

// The Vulkan image of x's kind at w x h (level 0), its memory, in the GENERAL layout and cleared (colour 0, depth 1)
static int img_make(vk_img *x, uint32_t w, uint32_t h, VkImage *out, VkDeviceMemory *mem) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, x->cflags, x->itype, x->fmt, {w, h, x->d}, x->levels, x->layers,
                            (VkSampleCountFlagBits)x->samples, VK_IMAGE_TILING_OPTIMAL, x->usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage im;
    VkResult r = vkCreateImage(g_vk.dev, &ci, NULL, &im);
    if (r != VK_SUCCESS) {
        VRED("vkCreateImage %ux%ux%u x%u levels %u fmt %d failed (%d)", w, h, x->d, x->layers, x->levels, x->fmt, r);
        return 0;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, im, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vk_mem_type(mr.memoryTypeBits, 0);
    if (vkAllocateMemory(g_vk.dev, &ai, NULL, mem) != VK_SUCCESS) {
        VRED("out of GPU memory for a %ux%u image (%llu MB)", w, h, (unsigned long long)(mr.size >> 20));
        vkDestroyImage(g_vk.dev, im, NULL);
        return 0;
    }
    VK_CHECK(vkBindImageMemory(g_vk.dev, im, *mem, 0));
    // everything lives in the GENERAL layout (valid for every use); fresh images start cleared to zero
    int once = 0;
    VkCommandBuffer cb = tex_cmd(&once);
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, im, {x->aspect, 0, x->levels, 0, x->layers}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageSubresourceRange rr = {x->aspect, 0, x->levels, 0, x->layers};
    const int compressed = (x->cflags & VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT) != 0;
    if (x->aspect == VK_IMAGE_ASPECT_COLOR_BIT && !compressed) {
        VkClearColorValue cc = {{0}};
        vkCmdClearColorImage(cb, im, VK_IMAGE_LAYOUT_GENERAL, &cc, 1, &rr);
    } else if (x->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearDepthStencilValue dv = {1.0f, 0};
        vkCmdClearDepthStencilImage(cb, im, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &rr);
    }
    if (once) vk_once_end(cb);
    else vk_barrier();
    *out = im;
    return 1;
}

// Makes the Vulkan image of a texture (scaled when it is a render target at the resolution scale), or a buffer view's
// stand-in.
static vk_img *img_new(const tex_t *t, uint64_t akey) {
    const vk_fmt *f = vk_format(t->d.format);
    if (!f) { VRED_ONCE("texture format %#x has no Vulkan counterpart (first case)", t->d.format); return NULL; }
    vk_img *x = calloc(1, sizeof *x);
    x->refs = 1, x->alias_key = akey, x->target = t->d.target, x->fmt = f->vk;
    if (t->d.target == 10) return x;   // a texture buffer: a buffer view, made on use
    VkImageType type; VkImageViewType vt; VkImageCreateFlags fl;
    tex_geom(&t->d, &type, &vt, &x->w, &x->h, &x->d, &x->layers, &fl);
    x->levels = t->d.levels ? t->d.levels : 1;
    x->samples = t->d.samples > 1 ? (uint32_t)t->d.samples : 1;
    x->aspect = f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0;
    if (f->stencil) x->aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!x->aspect) x->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatFeatureFlags ff = fmt_features(f->vk);
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && x->samples == 1) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(u & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) {
        VRED_ONCE("format %d (NVN %#x) is not usable on this GPU (first case)", f->vk, t->d.format);
    }
    if (!f->depth && !f->stencil) fl |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (f->bw > 1 && !f->decode) fl |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    x->usage = u, x->cflags = fl, x->itype = type, x->hw = x->w, x->hh = x->h;
    if (!img_make(x, x->w, x->h, &x->im, &x->m)) { free(x); return NULL; }
    VLOG(2, "image %ux%ux%u x%u levels %u samples %u fmt %d (NVN %#x) storage %p", x->w, x->h, x->d, x->layers, x->levels, x->samples, f->vk, t->d.format, (void *)t->mem);
    return x;
}
// Drops one reference to an image; the last one destroys it once the GPU is done with it.
static void img_unref(vk_img *x) {
    if (--x->refs > 0) return;
    if (x->alias_key) map_put(&g_by_alias, x->alias_key, NULL);
    VkImageView vs[24];
    for (int i = 0; i < x->nviews; i++) vs[i] = x->views[i].v;
    if (x->im) vk_defer_destroy_image(x->im, vs, x->nviews, x->m);
    if (x->bview) vkDestroyBufferView(g_vk.dev, x->bview, NULL);
    for (int i = 0; i < 4; i++) if (x->bview_raw[i]) vkDestroyBufferView(g_vk.dev, x->bview_raw[i], NULL);
    free(x);
}

static uint64_t alias_key(const tex_t *t) {   // storage address with the shape and the format's class
    const vk_fmt *f = vk_format(t->d.format);
    if (!t->mem || !f) return 0;
    uint64_t h = (uint64_t)(uintptr_t)t->mem;
    h = h * 1000003 ^ t->d.width; h = h * 1000003 ^ t->d.height; h = h * 1000003 ^ t->d.depth;
    h = h * 1000003 ^ t->d.levels; h = h * 1000003 ^ (uint64_t)t->d.target; h = h * 1000003 ^ (uint64_t)t->d.samples;
    h = h * 1000003 ^ (f->bpb | f->bw << 8 | f->bh << 16 | f->depth << 24 | (uint64_t)f->stencil << 25 | (uint64_t)f->decode << 26);
    return h ? h : 1;
}

void mtl_tex_init(const tex_t *t) { (void)t; }   // images are made on first use
// A texture the game finalizes leaves the table at once (a later lookup of its serial makes a new one), but is freed
// only two submissions later: the replay of a submission on another thread, or the window thread showing it, may hold
// it until then. Freed at once, a draw read freed memory (Breath of the Wild 1.9.0, after loading, in vk_tex_view)
static vk_tex **g_dead_tex[2];
static size_t g_ndead_tex[2], g_cdead_tex[2];
// The game freed a texture: its image is released at the end of the submission.
void mtl_tex_release(tex_t *t) {
    if (!vk_ready() || !t->serial) return;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = map_get(&g_by_serial, t->serial);
    if (x) {
        map_put(&g_by_serial, t->serial, NULL);
        if (g_ndead_tex[0] == g_cdead_tex[0]) g_dead_tex[0] = realloc(g_dead_tex[0], (g_cdead_tex[0] = g_cdead_tex[0] ? 2 * g_cdead_tex[0] : 64) * sizeof *g_dead_tex[0]);
        g_dead_tex[0][g_ndead_tex[0]++] = x;
    }
    pthread_mutex_unlock(&g_tex_mu);
}
// after each submission (vk_core.c): free what was released two submissions ago, age what was released since
void vk_tex_reap(void) {
    pthread_mutex_lock(&g_tex_mu);
    for (size_t i = 0; i < g_ndead_tex[1]; i++) {
        vk_tex *x = g_dead_tex[1][i];
        if (x->img) img_unref(x->img);
        free(x);
    }
    vk_tex **l = g_dead_tex[1];
    size_t c = g_cdead_tex[1];
    g_dead_tex[1] = g_dead_tex[0], g_ndead_tex[1] = g_ndead_tex[0], g_cdead_tex[1] = g_cdead_tex[0];
    g_dead_tex[0] = l, g_ndead_tex[0] = 0, g_cdead_tex[0] = c;
    pthread_mutex_unlock(&g_tex_mu);
}

// the bytes of level `level`, layer `layer`, linear, for upload: deswizzled block-linear, or the linear rows; ASTC
// decoded to RGBA8 when the GPU has no ASTC. Returns the buffer (malloc) and its row pitch in bytes
static uint8_t *level_bytes(const tex_t *t, const vk_fmt *f, unsigned level, unsigned layer, uint32_t w, uint32_t h, uint32_t dz, size_t *pitch, size_t *total) {
    uint32_t bw = f->bw, bh = f->bh, bx = (w + bw - 1) / bw, by = (h + bh - 1) / bh;
    size_t row = (size_t)bx * f->bpb;
    size_t n = row * by * dz;
    uint8_t *lin = malloc(n ? n : 1);
    if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        size_t stride = t->d.stride ? t->d.stride : row;
        const uint8_t *src = t->mem + nvn_tex_level_offset(&t->d, level);
        for (uint32_t y = 0; y < by * dz; y++) memcpy(lin + y * row, src + y * stride, row);
    } else if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, row)) {
        VRED_ONCE("deswizzling NVN format %#x target %d failed (first case)", t->d.format, t->d.target);
        memset(lin, 0, n);
    }
    if (f->decode == 1) {   // ASTC → RGBA8
        size_t out_n = (size_t)w * h * dz * 4;
        uint8_t *o = malloc(out_n);
        uint64_t t0 = nxl_perf_now();
        nxl_astc_decode(lin, n, w, h, dz, bw, bh, o);
        nxl_perf_add(NXL_PERF_ASTC, t0);
        free(lin);
        *pitch = (size_t)w * 4, *total = out_n;
        return o;
    }
    *pitch = row, *total = n;
    return lin;
}

// staging for uploads outside a submission (the submission's scratch memory belongs to its thread): buffers of their
// own, freed once the one-off work is done (callers hold g_tex_mu)
typedef struct { VkBuffer b; VkDeviceMemory m; } staging;
static staging g_stg[64];
static int g_nstg;
// A host-visible buffer for an upload (up to 64 per submission, freed when it completes).
static void *staging_alloc(VkDeviceSize n, VkBuffer *buf, VkDeviceSize *off) {
    if (g_nstg == 64) return NULL;
    staging *s = &g_stg[g_nstg++];
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n ? n : 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &s->b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, s->b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &s->m));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, s->b, s->m, 0));
    void *p = NULL;
    VK_CHECK(vkMapMemory(g_vk.dev, s->m, 0, VK_WHOLE_SIZE, 0, &p));
    *buf = s->b, *off = 0;
    return p;
}
static void staging_free(void) {
    for (int i = 0; i < g_nstg; i++) vkDestroyBuffer(g_vk.dev, g_stg[i].b, NULL), vkFreeMemory(g_vk.dev, g_stg[i].m, NULL);
    g_nstg = 0;
}

// Copies a texture's guest memory into its image (decoding ASTC on the CPU where needed), when the memory changed.
static void img_upload(vk_img *x, const tex_t *t) {
    { vk_tex tx = {*t, x}; vk_trace_tex("upload", &tx, NULL, 0); }
    const vk_fmt *f = vk_format(t->d.format);
    if (!f || !x->im || f->depth || f->stencil || x->samples > 1 || !t->mem || x->sc) return;
    uint64_t stamp = nvn_dirty_now(), t0 = nxl_perf_now();
    int tracked = nvn_dirty_tracked(t->mem, t->L.size ? t->L.size : 1);
    int once = 0;
    VkCommandBuffer cb = tex_cmd(&once);
    uint32_t layers = x->target == 2 ? 1 : x->layers;
    for (unsigned lv = 0; lv < x->levels; lv++) {
        uint32_t w = x->w >> lv ? x->w >> lv : 1, h = x->h >> lv ? x->h >> lv : 1, dz = x->d >> lv ? x->d >> lv : 1;
        for (unsigned ly = 0; ly < layers; ly++) {
            size_t pitch, n;
            uint8_t *lin = level_bytes(t, f, lv, ly, w, h, dz, &pitch, &n);
            VkBuffer sb; VkDeviceSize so;
            void *dst = once ? staging_alloc(n, &sb, &so) : vk_scratch(n, 16, &sb, &so);
            if (!dst) { free(lin); continue; }   // more levels and layers than one-off staging takes: the rest stays zero
            memcpy(dst, lin, n);
            free(lin);
            atomic_fetch_add_explicit(&nxl_perf_upload_bytes, n, memory_order_relaxed);
            uint32_t bw = f->decode ? 1 : f->bw;
            VkBufferImageCopy c = {so, (uint32_t)(pitch / (f->decode ? 4 : f->bpb)) * bw, 0, {VK_IMAGE_ASPECT_COLOR_BIT, lv, ly, 1}, {0, 0, 0}, {w, h, dz}};
            vkCmdCopyBufferToImage(cb, sb, x->im, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        }
    }
    if (once) vk_once_end(cb), staging_free();
    else vk_barrier();
    if (tracked) nvn_dirty_protect(t->mem, t->L.size);
    x->stamp = stamp, x->uploaded = 1;
    nxl_perf_add(NXL_PERF_UPLOAD, t0);
    VLOG(2, "upload %ux%u x%u levels %u NVN fmt %#x from %p", x->w, x->h, x->layers, x->levels, t->d.format, (void *)t->mem);
}

// ---------- resolution scale ----------
// NXL_RES_SCALE=1.5 / 2 / 2.5 / 3 / 4: what the game draws into is drawn that much larger (its render targets: the scene,
// its G-buffer, shadow maps, effect buffers), so the picture has more detail than the console's resolution, not just a
// larger copy of it. As in yuzu: a texture is made larger when it is first drawn into (vk_tex_rescale, from the pass),
// draws into it get scaled viewports and scissors, and the translated shaders (yuzu's rescaling pass) are told per
// texture whether it is scaled, so a texel fetch, a size query or a fragment position keeps meaning the game's pixels.
// Copies and CPU reads see the game's size. A texture a compute shader writes, or that is copied from memory, stays at
// 1x (noscale). NXL_RES_SCALE_MIN=<w>x<h>: the smallest target that is scaled (default 256x128; small lookup tables and
// render-to-texture icons keep their size)
static uint32_t g_rs_up = 1, g_rs_shift, g_rs_min_w = 256, g_rs_min_h = 128;
static int g_rs_on = -1;
atomic_uint vk_tex_gen;   // bumped when an image is replaced (rescaled): cached views (vk_draw.c) are looked up again
// Reads the resolution scale settings (NXL_RES_SCALE, NXL_RES_SCALE_MIN) once.
static void res_init(void) {
    if (g_rs_on >= 0) return;
    g_rs_on = 0;
    const char *e = getenv("NXL_RES_SCALE"), *m = getenv("NXL_RES_SCALE_MIN");
    if (m) { unsigned a, b; if (sscanf(m, "%ux%u", &a, &b) == 2) g_rs_min_w = a, g_rs_min_h = b; else if (sscanf(m, "%u", &a) == 1) g_rs_min_w = g_rs_min_h = a; }
    if (!e || !*e) return;
    double v = strtod(e, NULL);
    static const struct { double v; uint32_t up, shift; } k[] = {{1, 1, 0}, {1.5, 3, 1}, {2, 2, 0}, {2.5, 5, 1}, {3, 3, 0}, {4, 4, 0}};
    int i = 0;
    while (i < 6 && fabs(k[i].v - v) > 0.01) i++;
    if (i == 6) { VRED("NXL_RES_SCALE=%s: 1, 1.5, 2, 2.5, 3 or 4; drawing at the game's size", e); return; }
    g_rs_up = k[i].up, g_rs_shift = k[i].shift;
    g_rs_on = g_rs_up != 1u << g_rs_shift;
    if (g_rs_on) fprintf(stderr, "[vk] resolution scale %gx: render targets of at least %ux%u are drawn %gx larger (NXL_RES_SCALE)\n", v, g_rs_min_w, g_rs_min_h, v);
}
int vk_res_active(void) { res_init(); return g_rs_on; }
void vk_res_params(uint32_t *up, uint32_t *shift) { res_init(); *up = g_rs_up, *shift = g_rs_shift; }
uint32_t vk_res_up(uint32_t v) { return g_rs_on ? (uint32_t)(((uint64_t)v * g_rs_up) >> g_rs_shift) : v; }
int32_t vk_res_up_i(int32_t v) { return g_rs_on ? (int32_t)(((int64_t)v * g_rs_up) >> g_rs_shift) : v; }   // (negative offsets: arithmetic shift)
float vk_res_factor(void) { return g_rs_on ? (float)g_rs_up / (float)(1u << g_rs_shift) : 1.0f; }
int vk_tex_scaled(vk_tex *x) { return x && x->img && x->img->sc; }
void vk_tex_hextent(vk_tex *x, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d) {   // the Vulkan image's size at a level
    uint32_t W = x->img->hw >> level, H = x->img->hh >> level, D = x->img->d >> level;
    *w = W ? W : 1, *h = H ? H : 1, *d = D ? D : 1;
}
// may be drawn at the resolution scale: a 2D (or 2D array, multisample, rectangle) block-linear target of an uncompressed
// format, large enough, not used in a way the scale cannot follow
int vk_tex_res_eligible(vk_tex *x) {
    if (!vk_res_active() || !x || !x->img || !x->img->im) return 0;
    vk_img *im = x->img;
    const vk_fmt *f = vk_format(x->t.d.format);
    if (im->noscale || !f || f->bw != 1 || f->decode) return 0;
    if (x->t.d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) return 0;
    if (!(im->target == 1 || im->target == 4 || im->target == 5 || im->target == 6 || im->target == 7)) return 0;
    return im->w >= g_rs_min_w && im->h >= g_rs_min_h;
}
// The image remade at the scale (up) or at 1x, its contents carried over (scaled by a blit; multisample contents are
// dropped, as yuzu does). Views of the old image go with it. 1: it now is as asked
int vk_tex_rescale(vk_tex *x, int up) {
    if (!x || !x->img || !x->img->im) return 0;
    vk_img *im = x->img;
    up = up ? 1 : 0;
    if (im->sc == up) return 1;
    if (up && !vk_tex_res_eligible(x)) return 0;
    pthread_mutex_lock(&g_tex_mu);
    uint32_t nw = up ? vk_res_up(im->w) : im->w, nh = up ? vk_res_up(im->h) : im->h;
    VkImage ni; VkDeviceMemory nm;
    if (!img_make(im, nw, nh, &ni, &nm)) {
        if (up) im->noscale = 1;
        pthread_mutex_unlock(&g_tex_mu);
        return 0;
    }
    int once = 0;
    VkCommandBuffer cb = tex_cmd(&once);
    VkFormatFeatureFlags ff = fmt_features(im->fmt);
    int blit = im->samples == 1 && (ff & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (ff & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    if (blit) {
        const vk_fmt *f = vk_format(x->t.d.format);
        int linear = im->aspect == VK_IMAGE_ASPECT_COLOR_BIT && f && !f->integer && (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        for (uint32_t lv = 0; lv < im->levels; lv++) {
            int32_t sw = (int32_t)((im->hw >> lv) ? im->hw >> lv : 1), sh = (int32_t)((im->hh >> lv) ? im->hh >> lv : 1);
            int32_t dw = (int32_t)((nw >> lv) ? nw >> lv : 1), dh = (int32_t)((nh >> lv) ? nh >> lv : 1);
            VkImageBlit r = {{im->aspect, lv, 0, im->layers}, {{0, 0, 0}, {sw, sh, 1}}, {im->aspect, lv, 0, im->layers}, {{0, 0, 0}, {dw, dh, 1}}};
            vkCmdBlitImage(cb, im->im, VK_IMAGE_LAYOUT_GENERAL, ni, VK_IMAGE_LAYOUT_GENERAL, 1, &r, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        }
    }
    if (once) vk_once_end(cb);
    else vk_barrier();
    VkImageView vs[24];
    for (int i = 0; i < im->nviews; i++) vs[i] = im->views[i].v;
    vk_defer_destroy_image(im->im, vs, im->nviews, im->m);
    im->nviews = 0;
    im->im = ni, im->m = nm, im->hw = nw, im->hh = nh, im->sc = (uint8_t)up;
    atomic_fetch_add(&vk_tex_gen, 1);
    pthread_mutex_unlock(&g_tex_mu);
    static int nlog;
    if (nlog < 12) nlog++, fprintf(stderr, "[vk] %s %ux%u%s (format %d) at %ux%u%s\n", up ? "resolution scale: target" : "resolution scale: back to 1x:",
                                   im->w, im->h, im->layers > 1 ? " array" : "", im->fmt, nw, nh, nlog == 12 ? " (further ones not logged)" : "");
    return 1;
}
// kept at 1x from now on: what uses it cannot follow the scale (a compute shader writing it, a copy from memory into it)
void vk_tex_noscale(vk_tex *x, const char *why) {
    if (!x || !x->img) return;
    if (x->img->sc) {
        static int n;
        if (n < 8) n++, fprintf(stderr, "[vk] resolution scale: a %ux%u target goes back to 1x (%s)\n", x->img->w, x->img->h, why);
        vk_tex_rescale(x, 0);
    }
    x->img->noscale = 1;
}

// The Vulkan side of an NVN texture (made the first time; images shared between textures over the same memory),
// uploaded when asked.
vk_tex *vk_tex_get(const tex_t *t, int upload) {
    if (!t || !t->serial || !vk_ready()) return NULL;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = map_get(&g_by_serial, t->serial);
    if (!x) {
        x = calloc(1, sizeof *x);
        x->t = *t;
        uint64_t ak = alias_key(t);
        vk_img *im = ak ? map_get(&g_by_alias, ak) : NULL;
        if (im) {
            im->refs++;
            const vk_fmt *f = vk_format(t->d.format);
            static int n;
            if (f && f->vk != im->fmt && n < 8) n++, fprintf(stderr, "[vk] texture of NVN format %#x (Vulkan %d) over storage whose image holds format %d (%ux%u)\n", t->d.format, f->vk, im->fmt, im->w, im->h);
        } else if ((im = img_new(t, ak)) && ak) map_put(&g_by_alias, ak, im);
        x->img = im;
        map_put(&g_by_serial, t->serial, x);
    }
    if (upload && x->img && !x->img->gpu && x->img->im) {
        const vk_fmt *f = vk_format(t->d.format);
        size_t sz = t->L.size ? t->L.size : 1;
        if (f && !f->depth && !f->stencil && (!x->img->uploaded || nvn_dirty_since(t->mem, sz, x->img->stamp)))
            img_upload(x->img, &x->t);
    }
    pthread_mutex_unlock(&g_tex_mu);
    return x;
}
VkImage vk_tex_image(vk_tex *x) { return x && x->img ? x->img->im : VK_NULL_HANDLE; }
// NXL_TRACE_FMT=<NVN format>[,...]: how textures of those formats are used (each distinct use once, at most 300 lines):
// as render targets, sampled, as storage images, copied, cleared, uploaded
static int g_trace_fmts[8], g_trace_nf = -1;
// Whether a texture is one the trace asks about (NXL_TRACE_FMT formats, NXL_TRACE_SIZE sizes).
int vk_trace_wants(vk_tex *x, const view_t *v, int has_view) {
    if (g_trace_nf < 0) {
        g_trace_nf = 0;
        const char *e = getenv("NXL_TRACE_FMT");
        for (char *p = (char *)e; e && *p && g_trace_nf < 8;) { g_trace_fmts[g_trace_nf++] = (int)strtol(p, &p, 0); while (*p == ',' || *p == ' ') p++; }
    }
    static uint32_t sizes[8][2]; static int ns = -1;
    if (ns < 0) {   // NXL_TRACE_SIZE=1280x720,320x180: only textures of these sizes
        ns = 0;
        const char *e = getenv("NXL_TRACE_SIZE");
        for (char *p = (char *)e; e && *p && ns < 8;) {
            sizes[ns][0] = (uint32_t)strtoul(p, &p, 10);
            if (*p == 'x') p++;
            sizes[ns++][1] = (uint32_t)strtoul(p, &p, 10);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    if ((!g_trace_nf && !ns) || !x) return 0;
    int tf = x->t.d.format, vf = has_view && v->set_format ? v->format : tf, hit = !g_trace_nf;
    for (int i = 0; i < g_trace_nf; i++) hit |= g_trace_fmts[i] == tf || g_trace_fmts[i] == vf;
    if (hit && ns) {
        int sz = 0;
        for (int i = 0; i < ns; i++) sz |= x->t.d.width == sizes[i][0] && x->t.d.height == sizes[i][1];
        hit = sz;
    }
    return hit;
}
int vk_trace_active(void) {   // NXL_TRACE_FMT or NXL_TRACE_SIZE set
    static int on = -1;
    if (on < 0) on = getenv("NXL_TRACE_FMT") || getenv("NXL_TRACE_SIZE");
    return on;
}
// Logs an operation on a traced texture once (for debugging; up to 300 lines).
void vk_trace_tex(const char *op, vk_tex *x, const view_t *v, int has_view) {
    static int lines;
    static uint64_t seen[300];
    if (lines >= 300 || !vk_trace_wants(x, v, has_view)) return;
    int tf = x->t.d.format, vf = has_view && v->set_format ? v->format : tf;
    uint64_t k = (uint64_t)(uintptr_t)x->t.mem * 31 ^ (uint64_t)tf << 40 ^ (uint64_t)vf << 48 ^ (uint64_t)x->t.d.width << 20 ^ x->t.d.height;
    for (const char *c = op; *c; c++) k = k * 131 + (uint8_t)*c;
    for (int i = 0; i < lines; i++) if (seen[i] == k) return;
    seen[lines++] = k;
    int sw = has_view && v->set_swizzle;
    fprintf(stderr, "[vk] trace %-10s NVN format %#x%s%#x %ux%u levels %u storage %p, image format %d %ux%u, %s%s (%d %d %d %d)\n", op, tf,
            vf != tf ? " view " : " / ", vf, x->t.d.width, x->t.d.height, x->t.d.levels, (void *)x->t.mem, x->img ? x->img->fmt : -1,
            x->img ? x->img->w : 0, x->img ? x->img->h : 0, x->img && x->img->gpu ? "GPU-written" : "from memory", sw ? ", view mapping" : x->t.set_swz ? ", texture mapping" : ", mapping",
            sw ? v->swz[0] : x->t.set_swz ? x->t.swz & 7 : 2, sw ? v->swz[1] : x->t.set_swz ? x->t.swz >> 3 & 7 : 3,
            sw ? v->swz[2] : x->t.set_swz ? x->t.swz >> 6 & 7 : 4, sw ? v->swz[3] : x->t.set_swz ? x->t.swz >> 9 & 7 : 5);
}
// The textures the traced draws sample, written as pictures after the submission that drew them (into
// $NXL_TRACE_DIR, default ~/nxl_trace): RGBA8 / BGRA8 / RGB10A2 only, each storage and size once, at most 16
static tex_t g_dump_q[16]; static int g_dump_n, g_dump_done;
static uint64_t g_dump_seen[16];
// Queues a texture to be written to a PNG after the submission (NXL_DUMP_TEX, debugging).
void vk_trace_dump_later(const tex_t *t) {
    int f = t->d.format;
    if (f != 0x25 && f != 0x38 && f != 0x3d && f != 0x3f && f != 0x99 && f != 0x9a && f != 0x1 && f != 0x6 && f != 0xd && f != 0x5 && f != 0xa) return;
    if (t->d.target != 1 && t->d.target != 4) return;   // 2D and 2D array (their first layer)
    uint64_t k = (uint64_t)(uintptr_t)t->mem ^ (uint64_t)t->d.width << 40 ^ (uint64_t)t->d.height << 52;
    for (int i = 0; i < g_dump_done + g_dump_n && i < 16; i++) if (g_dump_seen[i] == k) return;
    if (g_dump_done + g_dump_n >= 16) return;
    g_dump_seen[g_dump_done + g_dump_n] = k;
    g_dump_q[g_dump_n++] = *t;
}
// Writes the queued textures to PNG files.
void vk_trace_dump_flush(void) {
    for (int i = 0; i < g_dump_n; i++) {
        const tex_t *t = &g_dump_q[i];
        uint32_t w = t->d.width, h = t->d.height;
        uint32_t *px = malloc((size_t)w * h * 8);
        NVNcopyRegion r = {0, 0, 0, (int)w, (int)h, 1};
        const vk_fmt *vf = vk_format(t->d.format);
        size_t bpp = vf ? vf->bpb : 4;
        mtl_read_texels(t, 0, 0, &r, (uint8_t *)px, (size_t)w * bpp, 0);
        const char *dir = getenv("NXL_TRACE_DIR"), *home = getenv("HOME");
        char path[600], d2[512];
        if (!dir) { snprintf(d2, sizeof d2, "%s/nxl_trace", home ? home : "."); dir = d2; }
        mkdir(dir, 0755);
        snprintf(path, sizeof path, "%s/tex_%02d_%ux%u_fmt%x_%llx.ppm", dir, g_dump_done, w, h, t->d.format, (unsigned long long)(uintptr_t)t->mem);
        FILE *o = fopen(path, "wb");
        if (o) {
            fprintf(o, "P6 %u %u 255\n", w, h);
            for (size_t k = 0; k < (size_t)w * h; k++) {
                uint32_t v = bpp == 4 ? px[k] : bpp == 2 ? ((uint16_t *)px)[k] : ((uint8_t *)px)[k];
                uint8_t c[3];
                if (t->d.format == 0xa) { float fv; memcpy(&fv, &v, 4); fv = fv / 64.0f; c[0] = c[1] = c[2] = (uint8_t)(fv <= 0 ? 0 : fv >= 1 ? 255 : fv * 255); }   // R32F: value / 64
                else if (t->d.format == 0x1) c[0] = c[1] = c[2] = (uint8_t)v;
                else if (t->d.format == 0x6) c[0] = c[1] = c[2] = (uint8_t)(v >> 8);   // R16 unorm: the top byte
                else if (t->d.format == 0x5) { _Float16 hf; uint16_t hv = (uint16_t)v; memcpy(&hf, &hv, 2); float fv = (float)hf; c[0] = c[1] = c[2] = (uint8_t)(fv <= 0 ? 0 : fv >= 1 ? 255 : fv * 255); }
                else if (t->d.format == 0xd) c[0] = (uint8_t)v, c[1] = (uint8_t)(v >> 8), c[2] = 0;
                else if (t->d.format == 0x3d) for (int j = 0; j < 3; j++) c[j] = (uint8_t)(((v >> (10 * j)) & 0x3FF) >> 2);
                else if (t->d.format == 0x3f) {   // R11G11B10F: 6 / 6 / 5 mantissa bits, 5 exponent bits each
                    float fl[3];
                    for (int j = 0; j < 3; j++) {
                        uint32_t bits = j == 0 ? v & 0x7FF : j == 1 ? v >> 11 & 0x7FF : v >> 22 & 0x3FF, mb = j == 2 ? 5 : 6;
                        uint32_t e = bits >> mb, m = bits & ((1u << mb) - 1);
                        fl[j] = e == 0 ? ldexpf((float)m / (float)(1u << mb), -14) : e == 31 ? 65504.0f : ldexpf(1.0f + (float)m / (float)(1u << mb), (int)e - 15);
                        c[j] = (uint8_t)(fl[j] >= 1.0f ? 255 : fl[j] * 255.0f + 0.5f);
                    }
                    if (w * h <= 256) fprintf(stderr, "[vk] trace: texel %zu of %ux%u R11G11B10F: %g %g %g\n", k, w, h, fl[0], fl[1], fl[2]);
                }
                else if (t->d.format == 0x99 || t->d.format == 0x9a) c[0] = (uint8_t)(v >> 16), c[1] = (uint8_t)(v >> 8), c[2] = (uint8_t)v;
                else c[0] = (uint8_t)v, c[1] = (uint8_t)(v >> 8), c[2] = (uint8_t)(v >> 16);
                fwrite(c, 1, 3, o);
            }
            fclose(o);
            fprintf(stderr, "[vk] trace: wrote %s\n", path);
        }
        free(px);
        g_dump_done++;
    }
    g_dump_n = 0;
}
VkFormat vk_tex_format(vk_tex *x) { return x && x->img ? x->img->fmt : VK_FORMAT_UNDEFINED; }
// the format the game means for this texture (or view): an image is shared by every texture over the same storage with a
// format of the same size, and keeps the format of the first, so this can differ from vk_tex_format
VkFormat vk_tex_nvn_format(vk_tex *x, const view_t *v, int has_view) {
    if (!x || !x->img) return VK_FORMAT_UNDEFINED;
    const vk_fmt *f = vk_format(has_view && v->set_format ? v->format : x->t.d.format);
    if (!f || f->decode || f->depth || f->stencil || f->bw != 1) return x->img->fmt;
    const vk_fmt *b = vk_format(x->t.d.format);
    if (b && (b->bpb != f->bpb || b->decode)) return x->img->fmt;
    return f->vk;
}
// single-level 2D images in a given format, for converting copies and clears into an image that holds another format;
// kept and reused (two slots: a copy's source and destination)
typedef struct { VkImage im; VkDeviceMemory m; VkFormat f; uint32_t w, h; int slot; uint64_t used; } tmp_img;
static tmp_img g_tmp[12];
static uint64_t g_tmp_clock;
// A temporary image of a format and size, from a small cache (the least recently used one replaced).
VkImage vk_tmp_image(VkFormat f, uint32_t w, uint32_t h, int slot) {
    tmp_img *lru = &g_tmp[0];
    for (int i = 0; i < 12; i++) {
        tmp_img *t = &g_tmp[i];
        if (t->im && t->f == f && t->w == w && t->h == h && t->slot == slot) { t->used = ++g_tmp_clock; return t->im; }
        if (t->used < lru->used) lru = t;
    }
    if (lru->im) { VkImageView none = VK_NULL_HANDLE; vk_defer_destroy_image(lru->im, &none, 0, lru->m); lru->im = VK_NULL_HANDLE; }
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {w, h, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(g_vk.dev, &ci, NULL, &lru->im) != VK_SUCCESS) { lru->im = VK_NULL_HANDLE; return VK_NULL_HANDLE; }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g_vk.dev, lru->im, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vk_mem_type(mr.memoryTypeBits, 0);
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &lru->m));
    VK_CHECK(vkBindImageMemory(g_vk.dev, lru->im, lru->m, 0));
    lru->f = f, lru->w = w, lru->h = h, lru->slot = slot, lru->used = ++g_tmp_clock;
    VkCommandBuffer cb = vk_cmd();   // into GENERAL, as every image here
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, lru->im, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
    return lru->im;
}
// a copy between these two image formats needs a conversion: different channel layouts or number types (an sRGB and a UNORM
// format of the same layout count as the same: NVN's copy moves their bytes unchanged)
static VkFormat srgb_to_unorm(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM; case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}
int vk_copy_converts(VkFormat s, VkFormat d) { return srgb_to_unorm(s) != srgb_to_unorm(d); }
// both are colour formats a blit can convert between (not integer, depth or compressed), and the device blits them
int vk_blit_ok(VkFormat s, VkFormat d) {
    for (int i = 0; i < 2; i++) {
        VkFormat f = i ? d : s;
        int bad = 0;
        for (int k = 1; k < (int)(sizeof T / sizeof *T); k++)
            if (T[k].vk == f && (T[k].integer || T[k].depth || T[k].stencil || T[k].bw != 1)) bad = 1;
        if (bad || f == VK_FORMAT_UNDEFINED) return 0;
    }
    return (fmt_features(s) & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (fmt_features(d) & VK_FORMAT_FEATURE_BLIT_DST_BIT);
}
// Linear textures (LINEAR / LINEAR_RENDER_TARGET) live in the game's memory on the Switch: what the GPU draws into one
// the CPU can read straight from its pool (nxnative makes them on the pool buffer itself). Here every texture is a
// Vulkan image of its own, so the CPU read stale memory: those written by the GPU in a submission are copied back to
// their storage at its end (vk_tex_writeback)
static vk_tex *g_wb[256]; static int g_nwb;
// Marks a texture as drawn by the GPU; a linear one is copied back to guest memory after the submission.
void vk_tex_mark_gpu(vk_tex *x) {
    if (!x || !x->img) return;
    x->img->gpu = 1;
    if (!(x->t.d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !x->t.mem) return;
    for (int i = 0; i < g_nwb; i++) if (g_wb[i] == x) return;
    if (g_nwb < 256) g_wb[g_nwb++] = x;
    else VRED_ONCE("more than 256 linear textures written in one submission: the rest are not copied back to memory");
}
// Copies the linear textures drawn this submission back to guest memory (the CPU reads them there).
void vk_tex_writeback(VkCommandBuffer cb) {
    if (!g_nwb || !cb) { g_nwb = 0; return; }
    int done = 0;
    for (int i = 0; i < g_nwb; i++) {
        vk_tex *x = g_wb[i];
        const tex_t *t = &x->t;
        const vk_fmt *f = vk_format(t->d.format);
        if (!f || f->bw != 1 || f->decode || f->depth || f->stencil || !x->img->im || x->img->samples > 1) continue;
        uint32_t w = x->img->w, h = x->img->h, layers = x->img->layers;
        size_t row = (size_t)w * f->bpb, stride = t->d.stride ? (size_t)t->d.stride : row;
        if (stride % f->bpb || stride < row) { VRED_ONCE("linear texture row pitch %zu is not a whole number of %u-byte texels: not copied back", stride, f->bpb); continue; }
        VkBuffer b; VkDeviceSize o;
        uint64_t n = (uint64_t)stride * (h - 1) + row;
        if (!vk_buf_at((uint64_t)(uintptr_t)t->mem, n, &b, &o) || o % f->bpb) { VRED_ONCE("a linear texture's storage (%ux%u NVN %#x) is not in a memory pool: not copied back", w, h, t->d.format); continue; }
        if (!done++) vk_barrier();
        VkBufferImageCopy c = {o, (uint32_t)(stride / f->bpb), h, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers > 1 ? 1 : 1}, {0, 0, 0}, {w, h, 1}};
        vkCmdCopyImageToBuffer(cb, x->img->im, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
        static int logged;
        if (logged < 4) logged++, fprintf(stderr, "[vk] linear texture written by the GPU copied back to memory: %ux%u NVN %#x at %p\n", w, h, t->d.format, (void *)t->mem);
    }
    if (done) vk_barrier();
    g_nwb = 0;
}
void vk_tex_extent(vk_tex *x, unsigned level, uint32_t *w, uint32_t *h, uint32_t *d) {
    uint32_t W = x->img->w >> level, H = x->img->h >> level, D = x->img->d >> level;
    *w = W ? W : 1, *h = H ? H : 1, *d = D ? D : 1;
}
uint32_t vk_tex_layers(vk_tex *x) { return x->img->layers; }
const tex_t *vk_tex_desc(vk_tex *x) { return &x->t; }
VkImageAspectFlags vk_tex_aspect(vk_tex *x) { return x->img->aspect; }
// Whether the GPU has drawn into a texture (the CPU's copy of it is then stale).
int mtl_tex_gpu_owned(uint64_t serial) {
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = map_get(&g_by_serial, serial);
    int r = x && x->img && x->img->gpu;
    pthread_mutex_unlock(&g_tex_mu);
    return r;
}
void mtl_tex_cpu_written(const tex_t *t) {   // the CPU wrote the storage (WriteTexels, a flush): upload it again on use
    if (!vk_ready()) return;
    pthread_mutex_lock(&g_tex_mu);
    vk_tex *x = map_get(&g_by_serial, t->serial);
    if (x && x->img) x->img->uploaded = 0, x->img->gpu = 0;
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_tex_flush_uploads(void) {}

static VkComponentSwizzle swz_of(int nvn) {   // NVN / Metal channel: 0 zero 1 one 2 R 3 G 4 B 5 A
    static const VkComponentSwizzle m[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R,
                                            VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
    return nvn >= 0 && nvn < 6 ? m[nvn] : VK_COMPONENT_SWIZZLE_IDENTITY;
}
// compose b after a: the result's channel i takes a's channel b[i]
static VkComponentSwizzle swz_pick(const VkComponentSwizzle a[4], VkComponentSwizzle b) {
    switch (b) {
    case VK_COMPONENT_SWIZZLE_R: return a[0]; case VK_COMPONENT_SWIZZLE_G: return a[1];
    case VK_COMPONENT_SWIZZLE_B: return a[2]; case VK_COMPONENT_SWIZZLE_A: return a[3];
    default: return b;
    }
}

// The shader translator's texture type of a texture (or its view).
int vk_tex_type_of(const tex_t *t, const view_t *v, int has_view) {
    int target = has_view && v->set_target ? v->target : t->d.target;
    switch (target) {
    case 0: return NXL_TT_1D; case 3: return NXL_TT_1D_ARRAY; case 2: return NXL_TT_3D;
    case 4: case 6: return NXL_TT_2D_ARRAY; case 8: return NXL_TT_CUBE; case 9: return NXL_TT_CUBE_ARRAY;
    case 10: return NXL_TT_BUFFER; case 7: return NXL_TT_2D_RECT;
    default: return NXL_TT_2D;
    }
}

static VkImageView tex_view(vk_tex *x, const view_t *v, int has_view, int for_storage, int for_attachment, int stencil, VkFormat force);
VkImageView vk_tex_view(vk_tex *x, const view_t *v, int has_view, int for_storage, int for_attachment, int stencil) {
    return tex_view(x, v, has_view, for_storage, for_attachment, stencil, VK_FORMAT_UNDEFINED);
}
// a storage image as the shader declares it: a typed (raw) declaration (SULD.D / SUST.D: R32_UINT and the like) reads and
// writes the bits, so the view takes that format; a typeless one, the texture's
VkImageView vk_tex_storage_view(vk_tex *x, const view_t *v, int has_view, int img_format) {
    static const VkFormat m[14] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
                                   VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
                                   VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const uint8_t size[14] = {0, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16, 16};
    VkFormat force = VK_FORMAT_UNDEFINED;
    if (x && img_format > 0 && img_format < 14) {
        const vk_fmt *f = vk_format(has_view && v->set_format ? v->format : x->t.d.format);
        if (f && f->bw == 1 && !f->decode && f->bpb == size[img_format]) force = m[img_format];
        else { static int n; if (n++ < 4) fprintf(stderr, "[vk] storage image declared with format %d over NVN format %#x of another size: the texture's format is used\n", img_format, x->t.d.format); }
    }
    return tex_view(x, v, has_view, 1, 0, 0, force);
}
// An image view of a texture for sampling, storage or drawing into: its format, levels, layers, swizzle (made once,
// kept).
static VkImageView tex_view(vk_tex *x, const view_t *v, int has_view, int for_storage, int for_attachment, int stencil, VkFormat force) {
    if (!x || !x->img || !x->img->im) return VK_NULL_HANDLE;
    vk_img *im = x->img;
    const tex_t *t = &x->t;
    int fmt = has_view && v->set_format ? v->format : t->d.format;
    const vk_fmt *f = vk_format(fmt);
    if (!f) return VK_NULL_HANDLE;
    VkFormat vf = f->vk;
    const vk_fmt *base = vk_format(t->d.format);
    if (base && (base->depth || base->stencil)) vf = im->fmt;   // depth views keep the image's format
    if (base && base->decode && !f->decode) vf = im->fmt;
    if (base && f->decode && f->srgb) vf = VK_FORMAT_R8G8B8A8_SRGB;
    if (force) vf = force;
    int target = has_view && v->set_target ? v->target : t->d.target;
    uint32_t bl = has_view && v->set_levels ? v->base_level : 0, nl = has_view && v->set_levels ? v->nlevels : im->levels;
    uint32_t b0 = has_view && v->set_layers ? v->min_layer : 0, nly = has_view && v->set_layers ? v->nlayers : im->layers;
    if (bl >= im->levels) bl = im->levels ? im->levels - 1 : 0;
    if (!nl || bl + nl > im->levels) nl = im->levels - bl;
    if (b0 >= im->layers) b0 = 0;
    if (!nly || b0 + nly > im->layers) nly = im->layers - b0;
    VkImageViewType vt;
    switch (target) {
    case 0: vt = VK_IMAGE_VIEW_TYPE_1D; nly = 1; break;
    case 3: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
    case 2: vt = for_attachment ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_3D; break;
    case 4: case 6: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
    case 8: vt = for_attachment || nly < 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE; if (!for_attachment && nly >= 6) nly = 6; break;
    case 9: vt = for_attachment || nly < 6 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; if (!for_attachment) nly = nly / 6 * 6; break;
    default: vt = nly > 1 && for_attachment ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; if (!for_attachment) nly = 1; break;
    }
    if (im->target == 2 && vt == VK_IMAGE_VIEW_TYPE_2D_ARRAY && for_attachment) {
        // a 3D level as a target: the slices the view names (one draw per slice, or several targets on several slices
        // at once: Breath of the Wild builds its 8x8x8 colour-grading table with eight targets, one per slice), all
        // slices without one. Ignoring the view's slice put every target on slice 0: the table came out empty and the
        // blurred, graded copy of the screen behind the menus nearly black with a little blue
        uint32_t depth = im->d >> bl ? im->d >> bl : 1;
        b0 = has_view && v->set_layers ? v->min_layer : 0;
        nly = has_view && v->set_layers && v->nlayers ? v->nlayers : depth - b0;
        if (b0 >= depth) b0 = depth - 1;
        if (b0 + nly > depth) nly = depth - b0;
    }
    if ((vt == VK_IMAGE_VIEW_TYPE_CUBE || vt == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY) && !(im->layers >= 6 && im->w == im->h)) vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    if (for_attachment) nl = 1;
    VkImageAspectFlags asp = im->aspect;
    if (!for_attachment && asp == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) asp = stencil || (t->stencil) || (has_view && v->set_dsmode && v->dsmode == 1) ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkComponentMapping cm = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    if (!for_storage && !for_attachment) {
        VkComponentSwizzle s[4] = {f->swz[0], f->swz[1], f->swz[2], f->swz[3]};
        if (asp == VK_IMAGE_ASPECT_DEPTH_BIT || asp == VK_IMAGE_ASPECT_STENCIL_BIT) s[1] = s[2] = s[3] = VK_COMPONENT_SWIZZLE_R, s[3] = VK_COMPONENT_SWIZZLE_ONE;
        // the view's channel mapping if it sets one, otherwise the texture's: a view's mapping replaces the texture's, both name
        // channels of the format (as nxnative's nvn_mtl.m). Composing them showed Breath of the Wild's menu backdrop (an
        // (R, 0, 0, 1) texture read through a view that reorders channels) as its red channel in blue
        int sw[4] = {-1, -1, -1, -1};
        if (has_view && v->set_swizzle) for (int i = 0; i < 4; i++) sw[i] = v->swz[i];
        else if (t->set_swz) for (int i = 0; i < 4; i++) sw[i] = t->swz >> (3 * i) & 7;
        if (sw[0] >= 0) {
            static uint32_t seen[16]; static int nseen;   // the first distinct mappings, for reports
            uint32_t k = (uint32_t)fmt << 16 ^ (uint32_t)(sw[0] | sw[1] << 3 | sw[2] << 6 | sw[3] << 9) ^ (uint32_t)(t->set_swz ? t->swz : 0xfff) << 20;
            int known = 0;
            for (int i = 0; i < nseen; i++) known |= seen[i] == k;
            if (!known && nseen < 16) {
                seen[nseen++] = k;
                fprintf(stderr, "[vk] channel mapping: format %#x, texture (%d %d %d %d)%s, used (%d %d %d %d)\n", fmt,
                        t->set_swz ? t->swz & 7 : 2, t->set_swz ? t->swz >> 3 & 7 : 3, t->set_swz ? t->swz >> 6 & 7 : 4, t->set_swz ? t->swz >> 9 & 7 : 5,
                        has_view && v->set_swizzle ? ", from a view" : "", sw[0], sw[1], sw[2], sw[3]);
            }
            VkComponentSwizzle o[4];
            for (int i = 0; i < 4; i++) o[i] = sw[i] > 5 ? s[i] : swz_pick(s, swz_of(sw[i]));
            memcpy(s, o, sizeof o);
        }
        cm = (VkComponentMapping){s[0], s[1], s[2], s[3]};
    }
    vk_trace_tex(for_attachment ? "target" : for_storage ? "storage" : "sampled", x, v, has_view);
    uint64_t key = (uint64_t)vf | (uint64_t)vt << 16 | (uint64_t)bl << 20 | (uint64_t)nl << 25 | (uint64_t)b0 << 30 | (uint64_t)nly << 42;
    key ^= (uint64_t)cm.r * 0x1000193 ^ (uint64_t)cm.g * 0x100000001B3ull ^ (uint64_t)cm.b * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)cm.a * 0x165667B19E3779F9ull;
    key ^= (uint64_t)asp << 58 ^ (uint64_t)for_storage << 62 ^ (uint64_t)for_attachment << 63;
    g_view_layers = nly;
    for (int i = 0; i < im->nviews; i++) if (im->views[i].key == key) return im->views[i].v;
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                      for_storage ? VK_IMAGE_USAGE_STORAGE_BIT : for_attachment ? (asp & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) : VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, 0, im->im, vt, vf, cm, {asp, bl, nl, b0, nly}};
    VkImageView view;
    VkResult r = vkCreateImageView(g_vk.dev, &ci, NULL, &view);
    if (r != VK_SUCCESS) { VRED_ONCE("vkCreateImageView failed (%d): fmt %d type %d (first case)", r, vf, vt); return VK_NULL_HANDLE; }
    if (im->nviews == 24) { VkImageView old = im->views[0].v; vk_defer_destroy_image(VK_NULL_HANDLE, &old, 1, VK_NULL_HANDLE); memmove(im->views, im->views + 1, 23 * sizeof *im->views); im->nviews--; }
    im->views[im->nviews++] = (view_ent){key, view, nly};
    return view;
}

// a texture buffer's view over the pool memory
// a texture buffer's view over the pool memory; a storage one declared with a format (raw SULD.D / SUST.D) in that format,
// of the same size (as storage images, vk_tex_storage_view)
VkBufferView vk_tex_buffer_view(vk_tex *x, int storage, int img_format) {
    if (!x || !x->img) return VK_NULL_HANDLE;
    const vk_fmt *f = vk_format(x->t.d.format);
    static const VkFormat m[14] = {VK_FORMAT_UNDEFINED, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT,
                                   VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT,
                                   VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const uint8_t size[14] = {0, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8, 16, 16, 16};
    int raw = storage && f && img_format > 0 && img_format < 14 && size[img_format] == f->bpb;
    if (!raw && x->img->bview) return x->img->bview;
    int slot = 0;
    if (raw) {
        for (slot = 0; slot < 4 && x->img->bview_raw[slot]; slot++) if (x->img->bview_raw_fmt[slot] == m[img_format]) return x->img->bview_raw[slot];
        if (slot == 4) raw = 0;   // more than four declarations: the texture's format
        if (!raw && x->img->bview) return x->img->bview;
    }
    VkBuffer b; VkDeviceSize o;
    uint64_t n = (uint64_t)x->t.d.width * (f ? f->bpb : 4);
    if (!f || !vk_buf_at((uint64_t)(uintptr_t)x->t.mem, n, &b, &o)) return VK_NULL_HANDLE;
    VkDeviceSize al = g_vk.props.limits.minTexelBufferOffsetAlignment;
    if (o % al) { VRED_ONCE("texture buffer at %p is not %llu-byte aligned", (void *)x->t.mem, (unsigned long long)al); return VK_NULL_HANDLE; }
    uint64_t max = (uint64_t)g_vk.props.limits.maxTexelBufferElements * f->bpb;
    VkBufferViewCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, b, raw ? m[img_format] : f->vk, o, n < max ? n : max};
    VkBufferView v;
    VK_CHECK(vkCreateBufferView(g_vk.dev, &ci, NULL, &v));
    if (!raw) x->img->bview = v;
    else x->img->bview_raw[slot] = v, x->img->bview_raw_fmt[slot] = m[img_format];
    return v;
}

// ---------- texture / sampler pool registrations ----------
#define REG_CHUNK 4096
static vk_reg_tex *g_rt[1 << 9];   // up to 2M ids, in chunks
static samp_par *g_rs[1 << 9];
static uint8_t *g_rs_used[1 << 9];
static VkSampler *g_rs_vk[1 << 9];   // the Vulkan sampler of each registration, made on first use
static VkSampler *g_rs_vk_plain[1 << 9];   // the same without the forced anisotropic filtering (vk_reg_sampler_for)
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
// The game registered a texture handle: kept for the shaders that sample it by handle.
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= REG_CHUNK << 9) return;
    pthread_mutex_lock(&g_reg_mu);
    vk_reg_tex **c = &g_rt[id / REG_CHUNK];
    if (!*c) *c = calloc(REG_CHUNK, sizeof **c);
    vk_reg_tex *e = &(*c)[id % REG_CHUNK];
    e->t = *t;
    if (v) e->v = *v; else memset(&e->v, 0, sizeof e->v);
    e->used = v ? 2 : 1, e->image = 0, e->csub = ~0ull, e->cx = NULL, e->cview = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_reg_mu);
}
void mtl_reg_image(int id, const tex_t *t, const view_t *v) {
    mtl_reg_tex(id, t, v);
    pthread_mutex_lock(&g_reg_mu);
    if (g_rt[id / REG_CHUNK]) g_rt[id / REG_CHUNK][id % REG_CHUNK].image = 1;
    pthread_mutex_unlock(&g_reg_mu);
}
const vk_reg_tex *vk_reg_tex_at(uint32_t id) {
    if (id >= REG_CHUNK << 9 || !g_rt[id / REG_CHUNK]) return NULL;
    const vk_reg_tex *e = &g_rt[id / REG_CHUNK][id % REG_CHUNK];
    return e->used ? e : NULL;
}
// The game registered a sampler handle.
void mtl_reg_samp(int id, const samp_par *p) {
    if (id < 0 || id >= REG_CHUNK << 9) return;
    pthread_mutex_lock(&g_reg_mu);
    if (!g_rs[id / REG_CHUNK]) g_rs[id / REG_CHUNK] = calloc(REG_CHUNK, sizeof(samp_par)), g_rs_used[id / REG_CHUNK] = calloc(REG_CHUNK, 1), g_rs_vk[id / REG_CHUNK] = calloc(REG_CHUNK, sizeof(VkSampler)), g_rs_vk_plain[id / REG_CHUNK] = calloc(REG_CHUNK, sizeof(VkSampler));
    g_rs[id / REG_CHUNK][id % REG_CHUNK] = *p;
    g_rs_vk[id / REG_CHUNK][id % REG_CHUNK] = VK_NULL_HANDLE;
    g_rs_vk_plain[id / REG_CHUNK][id % REG_CHUNK] = VK_NULL_HANDLE;
    g_rs_used[id / REG_CHUNK][id % REG_CHUNK] = 1;
    pthread_mutex_unlock(&g_reg_mu);
}
const samp_par *vk_reg_samp_at(uint32_t id) {
    if (id >= REG_CHUNK << 9 || !g_rs[id / REG_CHUNK] || !g_rs_used[id / REG_CHUNK][id % REG_CHUNK]) return NULL;
    return &g_rs[id / REG_CHUNK][id % REG_CHUNK];
}
static VkSampler sampler_ex(const samp_par *p, int plain);
VkSampler vk_reg_sampler(uint32_t id) { return vk_reg_sampler_for(id, 0); }
// plain: for a texture the GPU drew (a render target: shadow maps, the scene's buffers), which keeps the game's own
// filtering; NXL_ANISO forced onto those drew a line across Breath of the Wild's ground where its shadow cascades meet
VkSampler vk_reg_sampler_for(uint32_t id, int plain) {
    const samp_par *p = vk_reg_samp_at(id);
    if (!p) return vk_sampler(NULL);
    VkSampler *c = plain ? &g_rs_vk_plain[id / REG_CHUNK][id % REG_CHUNK] : &g_rs_vk[id / REG_CHUNK][id % REG_CHUNK];
    if (!*c) *c = sampler_ex(p, plain);
    return *c;
}
int vk_tex_gpu_drawn(vk_tex *x) { return x && x->img && x->img->gpu; }
void mtl_set_tex_pool_n(int n) { (void)n; }

// ---------- samplers ----------
typedef struct { samp_par p; int plain; VkSampler s; } samp_ent;
static samp_ent *g_samps;
static int g_nsamps, g_csamps;
// The Vulkan address mode of an NVN wrap mode.
static VkSamplerAddressMode wrap_of(int w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 0: case 7: default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
static VkCompareOp cmp_of(int f) {   // NVN compare / depth functions 1..8
    static const VkCompareOp m[9] = {VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
                                     VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return f >= 0 && f < 9 ? m[f] : VK_COMPARE_OP_ALWAYS;
}
VkCompareOp vk_compare_op(int f) { return cmp_of(f); }
VkSampler vk_sampler(const samp_par *p) { return sampler_ex(p, 0); }
// A Vulkan sampler for NVN sampler parameters (made once, kept); plain: without anisotropy.
static VkSampler sampler_ex(const samp_par *p, int plain) {
    if (!p) return g_vk.nearest;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < g_nsamps; i++) if (g_samps[i].plain == plain && !memcmp(&g_samps[i].p, p, sizeof *p)) { VkSampler s = g_samps[i].s; pthread_mutex_unlock(&mu); return s; }
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = p->magf == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = p->minf == 1 || p->minf == 3 || p->minf == 5 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = p->minf == 4 || p->minf == 5 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_of(p->ws), ci.addressModeV = wrap_of(p->wt), ci.addressModeW = wrap_of(p->wr);
    ci.mipLodBias = p->lod_bias;
    // NXL_ANISO=2/4/8/16 (the launcher's graphics option): at least that much anisotropic filtering on the game's
    // mipmapped linear samplers (textures seen at an angle, the ground, stay sharp); nearest-filtered ones (pixel art,
    // lookup tables) keep what the game asked for
    static int force_aniso = -1;
    if (force_aniso < 0) { const char *e = getenv("NXL_ANISO"); force_aniso = e ? atoi(e) : 0; if (force_aniso > 1) fprintf(stderr, "[vk] anisotropic filtering: at least %dx (NXL_ANISO)\n", force_aniso); }
    float aniso = p->aniso;
    // not on depth-compare samplers (shadow lookups) nor, plain, on textures the GPU drew: only the game's own textures
    if (force_aniso > 1 && !plain && p->cmp_mode != 1 && ci.minFilter == VK_FILTER_LINEAR && p->minf >= 2 && aniso < force_aniso) aniso = (float)force_aniso;
    ci.anisotropyEnable = aniso > 1 && g_vk.feat.samplerAnisotropy;
    ci.maxAnisotropy = aniso > 1 ? (aniso < g_vk.props.limits.maxSamplerAnisotropy ? aniso : g_vk.props.limits.maxSamplerAnisotropy) : 1;
    ci.compareEnable = p->cmp_mode == 1;
    ci.compareOp = cmp_of(p->cmp_func);
    ci.minLod = p->lod_min, ci.maxLod = p->minf <= 1 ? 0.25f : p->lod_max;   // no mipmap filter: level 0 only
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    VkSamplerCustomBorderColorCreateInfoEXT cb = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    VkSamplerReductionModeCreateInfo rm = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, NULL,
                                           p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : p->reduction == 2 ? VK_SAMPLER_REDUCTION_MODE_MAX : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    if (p->reduction) ci.pNext = &rm;
    const float *bc = p->border;
    if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 0) ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    else if (bc[0] == 0 && bc[1] == 0 && bc[2] == 0 && bc[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    else if (bc[0] == 1 && bc[1] == 1 && bc[2] == 1 && bc[3] == 1) ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    else if (g_vk.has_custom_border) {
        ci.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        memcpy(cb.customBorderColor.float32, bc, 16);
        cb.pNext = (void *)ci.pNext, ci.pNext = &cb;
    } else ci.borderColor = bc[3] > 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    VkSampler s;
    if (vkCreateSampler(g_vk.dev, &ci, NULL, &s) != VK_SUCCESS) s = g_vk.nearest;
    if (g_nsamps == g_csamps) g_samps = realloc(g_samps, (g_csamps = g_csamps ? 2 * g_csamps : 64) * sizeof *g_samps);
    g_samps[g_nsamps++] = (samp_ent){*p, plain, s};
    pthread_mutex_unlock(&mu);
    return s;
}

// ---------- CPU reads of texels (nvnTextureReadTexels) ----------
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    size_t h = r->height > 0 ? (size_t)r->height : 1, d = r->depth > 0 ? (size_t)r->depth : 1;
    vk_tex *x = vk_ready() ? vk_tex_get(t, 1) : NULL;
    const vk_fmt *f = vk_format(t->d.format);
    if (!x || !x->img || !x->img->im || !f || f->bw != 1 || f->decode) { memset(dst, 0, img ? img * d : row * h * d); return; }
    size_t w = r->width > 0 ? (size_t)r->width : 1, rb = w * f->bpb, n = rb * h * d;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, n, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer b; VkDeviceMemory m; void *p;
    VK_CHECK(vkCreateBuffer(g_vk.dev, &bi, NULL, &b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g_vk.dev, b, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, vk_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VK_CHECK(vkAllocateMemory(g_vk.dev, &ai, NULL, &m));
    VK_CHECK(vkBindBufferMemory(g_vk.dev, b, m, 0));
    int is3d = x->img->target == 2;
    VkBufferImageCopy c = {0, 0, 0, {x->img->aspect & VK_IMAGE_ASPECT_COLOR_BIT ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT, level, is3d ? 0 : layer0 + (uint32_t)r->zoffset, is3d ? 1 : (uint32_t)d},
                           {r->xoffset, r->yoffset, is3d ? r->zoffset : 0}, {(uint32_t)w, (uint32_t)h, is3d ? (uint32_t)d : 1}};
    VkCommandBuffer cb = vk_once_begin();
    VkImage tmp = VK_NULL_HANDLE; VkDeviceMemory tm = VK_NULL_HANDLE;
    if (x->img->sc) {   // drawn at the resolution scale: the region scaled down to the game's size first
        VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, x->img->fmt, {(uint32_t)w, (uint32_t)h, 1}, 1, c.imageSubresource.layerCount, VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
        VkMemoryRequirements tr;
        if (vkCreateImage(g_vk.dev, &ci, NULL, &tmp) == VK_SUCCESS) {
            vkGetImageMemoryRequirements(g_vk.dev, tmp, &tr);
            VkMemoryAllocateInfo ta = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, tr.size, vk_mem_type(tr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
            VK_CHECK(vkAllocateMemory(g_vk.dev, &ta, NULL, &tm));
            VK_CHECK(vkBindImageMemory(g_vk.dev, tmp, tm, 0));
            VkImageMemoryBarrier2 ib = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                        VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED,
                                        VK_QUEUE_FAMILY_IGNORED, tmp, {c.imageSubresource.aspectMask, 0, 1, 0, c.imageSubresource.layerCount}};
            VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &ib};
            vkCmdPipelineBarrier2(cb, &di);
            int32_t x0 = vk_res_up_i(r->xoffset), y0 = vk_res_up_i(r->yoffset), x1 = vk_res_up_i(r->xoffset + (int)w), y1 = vk_res_up_i(r->yoffset + (int)h);
            VkImageBlit bl = {c.imageSubresource, {{x0, y0, 0}, {x1, y1, 1}}, {c.imageSubresource.aspectMask, 0, 0, c.imageSubresource.layerCount}, {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}}};
            const int filt = !f->integer && x->img->aspect == VK_IMAGE_ASPECT_COLOR_BIT;
            vkCmdBlitImage(cb, x->img->im, VK_IMAGE_LAYOUT_GENERAL, tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, filt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
            VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
            VkDependencyInfo d2 = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb};
            vkCmdPipelineBarrier2(cb, &d2);
            c.imageSubresource.mipLevel = 0, c.imageSubresource.baseArrayLayer = 0, c.imageOffset = (VkOffset3D){0, 0, 0};
        } else tmp = VK_NULL_HANDLE;
    }
    vkCmdCopyImageToBuffer(cb, tmp ? tmp : x->img->im, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
    vk_once_end(cb);
    if (tmp) vkDestroyImage(g_vk.dev, tmp, NULL), vkFreeMemory(g_vk.dev, tm, NULL);
    VK_CHECK(vkMapMemory(g_vk.dev, m, 0, VK_WHOLE_SIZE, 0, &p));
    for (size_t z = 0; z < d; z++)
        for (size_t y = 0; y < h; y++) memcpy(dst + z * (img ? img : row * h) + y * row, (uint8_t *)p + (z * h + y) * rb, rb);
    vkDestroyBuffer(g_vk.dev, b, NULL);
    vkFreeMemory(g_vk.dev, m, NULL);
}
