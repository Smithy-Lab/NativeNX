// SPDX-License-Identifier: GPL-3.0-or-later
// NVN shaders (Maxwell machine code) to SPIR-V at run time, with yuzu's shader recompiler (third_party/shader_recompiler).
// A C interface for the Vulkan replay layer (nvn_vk.c): a program's stages are translated together, since a stage's
// inputs follow the previous stage's outputs and the descriptor bindings run on across the stages (one set, binding
// numbers in yuzu's order: per stage its uniform buffers, storage buffers, texture buffers, image buffers, textures,
// images). What the translation reads of the draw state (the texture handles in c2 and the types of the textures they
// name) goes through callbacks; the result records it, so a cached translation is reused only while those reads would
// give the same answers.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

// NVN's stage order (NVNshaderStage): vertex, tessellation control, tessellation evaluation, geometry, fragment, compute
enum { NXL_SH_VS, NXL_SH_TCS, NXL_SH_TES, NXL_SH_GS, NXL_SH_FS, NXL_SH_CS, NXL_SH_NSTAGE };

typedef struct {
    const uint8_t *code;      // code area: a 0x30-byte header, then (graphics) the 0x50-byte SPH and the instructions
    uint32_t code_len;
    const uint8_t *control;   // control section (0x880 bytes): +0x700 c1 offset in the code area, +0x704 code length,
                              // +0x748 compute workgroup size, +0x754 compute shared memory bytes
} nxl_sh_src;

// texture types (yuzu's TextureType)
enum { NXL_TT_1D, NXL_TT_1D_ARRAY, NXL_TT_2D, NXL_TT_2D_ARRAY, NXL_TT_3D, NXL_TT_CUBE, NXL_TT_CUBE_ARRAY, NXL_TT_BUFFER, NXL_TT_2D_RECT };

typedef struct {
    // c[index][offset] as the draw has it (the driver's segments: c2 the texture handles), 0 when unknown
    uint32_t (*cbuf)(void *ud, int stage, uint32_t index, uint32_t offset);
    // the texture a handle names: its type (NXL_TT_*), whether its format is an integer one / a signed integer one,
    // and its yuzu TexturePixelFormat (for typeless image loads; -1 = A8B8G8R8_UNORM)
    int (*tex_type)(void *ud, int stage, uint32_t handle, int *is_int, int *is_signed, int *pixfmt);
    void *ud;
} nxl_sh_env;

// What the device offers (filled from the Vulkan device)
typedef struct {
    uint32_t spirv_version;   // 0x00010000 | minor << 8
    uint8_t int8, int16, int64, float16, float64, storage8, storage16, uniform8, uniform16;
    uint8_t demote, vote, viewport_layer, multi_viewport, storage_image_ext_formats, typeless_loads;
    uint8_t warp_larger;      // subgroups may be larger than Maxwell's 32 lanes
    uint32_t min_ssbo_align;
    uint8_t nvidia;           // the proprietary NVIDIA driver (fp16 float controls are broken there)
} nxl_sh_caps;

// The draw state a translation depends on
typedef struct {
    uint8_t attr_type[32];    // vertex attribute component types: 0 float, 1 signed int, 2 unsigned int, 5 disabled
    uint8_t frag_type[8];     // render target component types: 0 float, 1 signed int, 2 unsigned int
    uint8_t topology;         // geometry input: 0 points, 1 lines, 2 lines adjacency, 3 triangles, 4 triangles adjacency
    uint8_t gl_depth;         // NVN depth mode near=-w (GL, -1..1): the last vertex stage maps z to Vulkan's 0..1
    uint8_t points;           // the draw is of points: the point size is fixed_point_size unless the shader writes it
    float point_size;
} nxl_sh_state;

enum { NXL_DK_UBO, NXL_DK_SSBO, NXL_DK_TEXBUF, NXL_DK_IMGBUF, NXL_DK_TEX, NXL_DK_IMG };

// One descriptor binding, in binding order. For textures and images the handle is found as yuzu's GetTextureHandle
// does (nxl_sh_desc_handle); count > 1 is an array indexed at run time (handle i at cbuf_offset + i * 4 << size_shift).
typedef struct {
    uint8_t kind, stage, is_written, is_read, is_integer, is_signed, is_depth, is_multisample;
    uint8_t has_secondary, indirect, tex_type, img_format;
    uint32_t binding, count;
    uint32_t cbuf_index, cbuf_offset;   // UBO: cbuf_index is the constant segment (c0..c17), cbuf_offset 0
    uint32_t shift_left, secondary_cbuf_index, secondary_cbuf_offset, secondary_shift_left, size_shift;
    uint32_t shift_right, mask, secondary_shift_right, secondary_mask;
    uint32_t table_cbuf_index, table_scale, table_base;
    uint32_t ubo_used;                    // UBO: the bytes the shader reads from its start (0: unknown)
    uint32_t scale_index;                 // texture / image: its bit in the rescaling push constant (textures, images counted apart)
} nxl_sh_desc;

// A recorded read of the draw state: a c[index][offset] word, or (kind 1) the type of the texture a handle names
typedef struct { uint8_t kind, stage; uint32_t index, offset, value; } nxl_sh_read;

typedef struct {
    uint32_t *spirv[NXL_SH_NSTAGE];
    size_t words[NXL_SH_NSTAGE];
    uint32_t stages;                       // bit per stage with SPIR-V
    nxl_sh_desc *descs; uint32_t ndescs;
    nxl_sh_read *reads; uint32_t nreads;
    uint32_t local[3], shared_bytes;       // compute
    uint8_t uses_render_area;              // a push constant at offset 0: the render area (x, y, w, h) as floats
    uint8_t uses_rescaling;                // push constants (resolution scale): 0 texture bits u32[4], 16 image bits u32[2], 24 float down factor
    uint8_t vs_reads_attr[32];             // the vertex stage reads generic attribute i
    uint8_t fs_writes[8];                  // the fragment stage writes render target i
    uint8_t writes_depth, uses_point_size;
    char err[512];
} nxl_sh_result;

void nxl_sh_set_caps(const nxl_sh_caps *caps);
// the resolution scale (up / 2^shift; 1 / 0 = none): translations then go through yuzu's rescaling pass
void nxl_sh_set_scale(uint32_t up, uint32_t shift);
// src: one entry per NVN stage (code NULL = absent); returns 0 on success
int nxl_sh_translate(const nxl_sh_src src[NXL_SH_NSTAGE], int compute, const nxl_sh_state *st, const nxl_sh_env *env, nxl_sh_result *out);
void nxl_sh_free(nxl_sh_result *r);
// the handle a texture / image descriptor names, element i of its array (as the shader computes it)
uint32_t nxl_sh_desc_handle(const nxl_sh_desc *d, uint32_t i, const nxl_sh_env *env);

#ifdef __cplusplus
}
#endif
