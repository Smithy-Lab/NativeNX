// SPDX-License-Identifier: GPL-3.0-or-later
// NVN shaders to SPIR-V with yuzu's shader recompiler (see nxl_shader.h). The flow follows yuzu's Vulkan pipeline cache
// (video_core/renderer_vulkan/vk_pipeline_cache.cpp): an Environment per stage over the NVN code area, Flow::CFG,
// TranslateProgram, then ConvertLegacyToGeneric and EmitSPIRV stage by stage with the runtime info of the draw and one
// Bindings counter for the whole program.
#include "nxl_shader.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "common/settings.h"

namespace {

nxl_sh_caps g_caps = {0x00010300, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 1, 16, 0};

constexpr uint32_t GFX_MAGIC = 0x12345678, CS_MAGIC = 0x12345679, CODE_HEADER = 0x30;

uint32_t rd32(const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

Shader::Stage yuzu_stage(int s) {
    switch (s) {
    case NXL_SH_VS: return Shader::Stage::VertexB;
    case NXL_SH_TCS: return Shader::Stage::TessellationControl;
    case NXL_SH_TES: return Shader::Stage::TessellationEval;
    case NXL_SH_GS: return Shader::Stage::Geometry;
    case NXL_SH_FS: return Shader::Stage::Fragment;
    default: return Shader::Stage::Compute;
    }
}

// One NVN stage's code area as yuzu's Environment: instructions by byte address from the code area's start, the SPH
// after the 0x30-byte header, c1 (the compiler's constants, BRX jump tables) from the code area at control +0x700, the
// other segments and the texture types through the callbacks, every answer recorded
class NvnEnv final : public Shader::Environment {
public:
    NvnEnv(const nxl_sh_src &s, int nvn_stage, const nxl_sh_env *e, std::vector<nxl_sh_read> *reads)
        : src{s}, nstage{nvn_stage}, env{e}, log{reads} {
        stage = yuzu_stage(nvn_stage);
        const uint32_t magic = s.code_len >= 4 ? rd32(s.code) : 0;
        if (stage == Shader::Stage::Compute) {
            start_address = magic == CS_MAGIC || magic == GFX_MAGIC ? CODE_HEADER : 0;   // SDK 10's compute code has no header
        } else {
            start_address = CODE_HEADER;
            if (s.code_len >= CODE_HEADER + sizeof(sph)) std::memcpy(&sph, s.code + CODE_HEADER, sizeof(sph));
        }
        if (s.control) {
            const uint32_t c1 = rd32(s.control + 0x700);
            if (c1 < s.code_len) c1_off = c1;
            for (int i = 0; i < 3; i++) local[i] = std::max(1u, rd32(s.control + 0x748 + 4 * i));
            shared = rd32(s.control + 0x754);
        }
        is_proprietary_driver = true;   // the game's shaders come from NVN's compiler (yuzu folds their c1 constants)
    }
    ~NvnEnv() override = default;

    u64 ReadInstruction(u32 address) override {
        if (address + 8 > src.code_len) return 0;
        u64 v;
        std::memcpy(&v, src.code + address, 8);
        return v;
    }
    u32 ReadCbufValue(u32 index, u32 offset) override {
        if (index == 1) {   // the shader's own constant segment
            if (c1_off && c1_off + offset + 4 <= src.code_len) return rd32(src.code + c1_off + offset);
            return 0;
        }
        const u32 v = env && env->cbuf ? env->cbuf(env->ud, nstage, index, offset) : 0;
        log->push_back({0, (uint8_t)nstage, index, offset, v});
        return v;
    }
    Shader::TextureType ReadTextureType(u32 handle) override {
        int is_int = 0, is_signed = 0, fmt = -1;
        const int t = env && env->tex_type ? env->tex_type(env->ud, nstage, handle, &is_int, &is_signed, &fmt) : NXL_TT_2D;
        log->push_back({1, (uint8_t)nstage, handle, 0, (uint32_t)t});
        return static_cast<Shader::TextureType>(t);
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override {
        int is_int = 0, is_signed = 0, fmt = -1;
        if (env && env->tex_type) env->tex_type(env->ud, nstage, handle, &is_int, &is_signed, &fmt);
        log->push_back({2, (uint8_t)nstage, handle, 0, (uint32_t)fmt});
        return fmt < 0 ? Shader::TexturePixelFormat::A8B8G8R8_UNORM : static_cast<Shader::TexturePixelFormat>(fmt);
    }
    bool IsTexturePixelFormatInteger(u32 handle) override {
        int is_int = 0, is_signed = 0, fmt = -1;
        if (env && env->tex_type) env->tex_type(env->ud, nstage, handle, &is_int, &is_signed, &fmt);
        log->push_back({3, (uint8_t)nstage, handle, 0, (uint32_t)is_int});
        return is_int != 0;
    }
    bool IsTexturePixelFormatSignedInteger(u32 handle) override {
        int is_int = 0, is_signed = 0, fmt = -1;
        if (env && env->tex_type) env->tex_type(env->ud, nstage, handle, &is_int, &is_signed, &fmt);
        log->push_back({4, (uint8_t)nstage, handle, 0, (uint32_t)is_signed});
        return is_signed != 0;
    }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 2; }   // the NVN driver keeps the bound handles in c2
    u32 LocalMemorySize() const override {
        return stage == Shader::Stage::Compute ? 0x1000 : static_cast<u32>(sph.LocalMemorySize());
    }
    u32 SharedMemorySize() const override { return shared; }
    std::array<u32, 3> WorkgroupSize() const override { return {local[0], local[1], local[2]}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

private:
    nxl_sh_src src;
    int nstage;
    const nxl_sh_env *env;
    std::vector<nxl_sh_read> *log;
    uint32_t c1_off{}, shared{};
    uint32_t local[3]{1, 1, 1};
};

Shader::Profile make_profile() {
    const nxl_sh_caps &c = g_caps;
    Shader::Profile p{};
    p.supported_spirv = c.spirv_version;
    p.unified_descriptor_binding = true;
    p.has_split_descriptor_sets = false;
    p.support_descriptor_aliasing = true;
    p.support_int8 = c.int8;
    p.support_int16 = c.int16;
    p.support_int8_storage = c.storage8;
    p.support_int16_storage = c.storage16;
    p.support_int8_uniform = c.uniform8;
    p.support_int16_uniform = c.uniform16;
    p.support_int64 = c.int64;
    p.support_storage_image_extended_formats = c.storage_image_ext_formats;
    p.support_vertex_instance_id = false;
    p.support_vote = c.vote;
    p.support_fragment_quad_swap = false;   // upstream sirit has no OpGroupNonUniformQuadSwap (see emit_spirv_warp.cpp)
    p.support_viewport_index_layer_non_geometry = c.viewport_layer;
    p.support_typeless_image_loads = c.typeless_loads;
    p.support_demote_to_helper_invocation = c.demote;
    p.support_derivative_control = true;
    p.support_native_ndc = false;
    p.support_scaled_attributes = false;
    p.support_multi_viewport = c.multi_viewport;
    p.warp_size_potentially_larger_than_guest = c.warp_larger;
    p.lower_left_origin_mode = false;
    p.has_broken_fp16_float_controls = c.nvidia;
    p.min_ssbo_alignment = c.min_ssbo_align;
    p.max_user_clip_distances = 8;
    return p;
}

Shader::HostTranslateInfo make_host_info() {
    Shader::HostTranslateInfo h{};
    h.support_float64 = g_caps.float64;
    h.support_float16 = g_caps.float16;
    h.support_int64 = g_caps.int64;
    h.support_snorm_render_buffer = true;
    h.support_viewport_index_layer = g_caps.viewport_layer;
    h.min_ssbo_alignment = g_caps.min_ssbo_align;
    return h;
}

void put_desc(std::vector<nxl_sh_desc> &out, nxl_sh_desc d) { out.push_back(d); }

// the descriptors of one stage, given the binding its first one takes (yuzu's order, see spirv_emit_context.cpp)
uint32_t collect(const Shader::Info &info, int nstage, uint32_t binding, std::vector<nxl_sh_desc> &out, uint32_t &tex_scale, uint32_t &img_scale) {
    for (const auto &d : info.constant_buffer_descriptors)
        for (uint32_t i = 0; i < d.count; i++) {
            nxl_sh_desc e{};
            e.kind = NXL_DK_UBO, e.stage = (uint8_t)nstage, e.binding = binding++, e.count = 1, e.cbuf_index = d.index + i;
            if (d.index + i < info.constant_buffer_used_sizes.size()) e.ubo_used = info.constant_buffer_used_sizes[d.index + i];
            put_desc(out, e);
        }
    for (const auto &d : info.storage_buffers_descriptors)
        for (uint32_t i = 0; i < d.count; i++) {
            nxl_sh_desc e{};
            e.kind = NXL_DK_SSBO, e.stage = (uint8_t)nstage, e.binding = binding++, e.count = 1, e.cbuf_index = d.cbuf_index;
            e.cbuf_offset = d.cbuf_offset + 16 * i, e.is_written = d.is_written;
            put_desc(out, e);
        }
    for (const auto &d : info.texture_buffer_descriptors) {
        nxl_sh_desc e{};
        e.kind = NXL_DK_TEXBUF, e.stage = (uint8_t)nstage, e.binding = binding++, e.count = d.count;
        e.is_integer = d.is_integer, e.is_signed = d.is_signed, e.has_secondary = d.has_secondary;
        e.cbuf_index = d.cbuf_index, e.cbuf_offset = d.cbuf_offset, e.shift_left = d.shift_left;
        e.secondary_cbuf_index = d.secondary_cbuf_index, e.secondary_cbuf_offset = d.secondary_cbuf_offset;
        e.secondary_shift_left = d.secondary_shift_left, e.size_shift = d.size_shift, e.shift_right = d.shift_right;
        e.mask = d.mask, e.secondary_shift_right = d.secondary_shift_right, e.secondary_mask = d.secondary_mask;
        e.indirect = d.indirect, e.table_cbuf_index = d.table_cbuf_index, e.table_scale = d.table_scale, e.table_base = d.table_base;
        e.tex_type = NXL_TT_BUFFER;
        put_desc(out, e);
    }
    for (const auto &d : info.image_buffer_descriptors) {
        nxl_sh_desc e{};
        e.kind = NXL_DK_IMGBUF, e.stage = (uint8_t)nstage, e.binding = binding++, e.count = d.count;
        e.is_written = d.is_written, e.is_read = d.is_read, e.is_integer = d.is_integer;
        e.cbuf_index = d.cbuf_index, e.cbuf_offset = d.cbuf_offset, e.size_shift = d.size_shift, e.mask = 0xFFFFFFFF;
        e.secondary_mask = 0xFFFFFFFF, e.img_format = (uint8_t)d.format, e.tex_type = NXL_TT_BUFFER;
        put_desc(out, e);
    }
    for (const auto &d : info.texture_descriptors) {
        nxl_sh_desc e{};
        e.kind = NXL_DK_TEX, e.stage = (uint8_t)nstage, e.binding = binding++, e.count = d.count;
        e.tex_type = (uint8_t)d.type, e.is_depth = d.is_depth, e.is_multisample = d.is_multisample;
        e.is_integer = d.is_integer, e.is_signed = d.is_signed, e.has_secondary = d.has_secondary;
        e.cbuf_index = d.cbuf_index, e.cbuf_offset = d.cbuf_offset, e.shift_left = d.shift_left;
        e.secondary_cbuf_index = d.secondary_cbuf_index, e.secondary_cbuf_offset = d.secondary_cbuf_offset;
        e.secondary_shift_left = d.secondary_shift_left, e.size_shift = d.size_shift, e.shift_right = d.shift_right;
        e.mask = d.mask, e.secondary_shift_right = d.secondary_shift_right, e.secondary_mask = d.secondary_mask;
        e.indirect = d.indirect, e.table_cbuf_index = d.table_cbuf_index, e.table_scale = d.table_scale, e.table_base = d.table_base;
        e.scale_index = tex_scale++;   // as DefineTextures counts (one per descriptor, arrays included)
        put_desc(out, e);
    }
    for (const auto &d : info.image_descriptors) {
        nxl_sh_desc e{};
        e.kind = NXL_DK_IMG, e.stage = (uint8_t)nstage, e.binding = binding++, e.count = d.count;
        e.tex_type = (uint8_t)d.type, e.img_format = (uint8_t)d.format, e.is_written = d.is_written, e.is_read = d.is_read;
        e.is_integer = d.is_integer, e.cbuf_index = d.cbuf_index, e.cbuf_offset = d.cbuf_offset, e.size_shift = d.size_shift;
        e.mask = 0xFFFFFFFF, e.secondary_mask = 0xFFFFFFFF;
        e.scale_index = img_scale++;
        put_desc(out, e);
    }
    return binding;
}

Shader::RuntimeInfo runtime_info(const Shader::IR::Program &prog, const Shader::IR::Program *prev, const nxl_sh_state *st, bool last_vertex_stage) {
    Shader::RuntimeInfo info;
    if (prev) {
        info.previous_stage_stores = prev->info.stores;
        info.previous_stage_legacy_stores_mapping = prev->info.legacy_stores_mapping;
        if (prev->is_geometry_passthrough) info.previous_stage_stores.mask |= prev->info.passthrough.mask;
    } else {
        info.previous_stage_stores.mask.set();
    }
    switch (prog.stage) {
    case Shader::Stage::VertexB:
        for (int i = 0; i < 32; i++) info.generic_input_types[i] = static_cast<Shader::AttributeType>(st ? st->attr_type[i] : 0);
        if (last_vertex_stage) {
            info.convert_depth_mode = st && st->gl_depth;
            if (st && st->points) info.fixed_state_point_size = st->point_size;
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_primitive = Shader::TessPrimitive::Triangles;
        info.tess_spacing = Shader::TessSpacing::Equal;
        if (last_vertex_stage) info.convert_depth_mode = st && st->gl_depth;
        break;
    case Shader::Stage::Geometry:
        info.convert_depth_mode = st && st->gl_depth;
        if (prog.output_topology == Shader::OutputTopology::PointList && st) info.fixed_state_point_size = st->point_size;
        break;
    case Shader::Stage::Fragment:
        for (int i = 0; i < 8; i++) info.frag_color_types[i] = static_cast<Shader::FragmentOutputType>(st ? st->frag_type[i] : 0);
        break;
    default:
        break;
    }
    info.input_topology = static_cast<Shader::InputTopology>(st ? st->topology : 3);
    return info;
}

} // namespace

extern "C" void nxl_sh_set_caps(const nxl_sh_caps *caps) { g_caps = *caps; }
extern "C" void nxl_sh_set_scale(uint32_t up, uint32_t shift) {
    auto &ri = Settings::values.resolution_info;
    ri.up_scale = up ? up : 1, ri.down_shift = shift;
    ri.up_factor = (float)ri.up_scale / (float)(1u << shift), ri.down_factor = 1.0f / ri.up_factor;
    ri.active = ri.up_scale != (1u << shift);
}

extern "C" void nxl_sh_free(nxl_sh_result *r) {
    for (auto &s : r->spirv) std::free(s), s = nullptr;
    std::free(r->descs), r->descs = nullptr, r->ndescs = 0;
    std::free(r->reads), r->reads = nullptr, r->nreads = 0;
}

extern "C" int nxl_sh_translate(const nxl_sh_src src[NXL_SH_NSTAGE], int compute, const nxl_sh_state *st, const nxl_sh_env *env, nxl_sh_result *out) {
    std::memset(out, 0, sizeof *out);
    std::vector<nxl_sh_read> reads;
    std::vector<nxl_sh_desc> descs;
    try {
        Shader::IR::InstStorage inst_pool;
        Shader::ObjectPool<Shader::IR::Block> block_pool{32};
        Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool{32};
        const Shader::Profile profile = make_profile();
        const Shader::HostTranslateInfo host = make_host_info();
        Shader::IR::Program programs[NXL_SH_NSTAGE];
        std::optional<NvnEnv> envs[NXL_SH_NSTAGE];
        int first = compute ? NXL_SH_CS : NXL_SH_VS, last = compute ? NXL_SH_CS : NXL_SH_FS;
        int last_vertex = -1;
        for (int s = first; s <= last; s++) {
            if (!src[s].code) continue;
            envs[s].emplace(src[s], s, env, &reads);
            NvnEnv &e = *envs[s];
            const uint32_t cfg_start = compute ? e.StartAddress() : e.StartAddress() + (uint32_t)sizeof(Shader::ProgramHeader);
            Shader::Maxwell::Flow::CFG cfg(e, flow_pool, cfg_start, false);
            programs[s] = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, e, cfg, host);
            if (s != NXL_SH_FS && s != NXL_SH_CS) last_vertex = s;
        }
        Shader::Backend::Bindings bindings;
        const Shader::IR::Program *prev = nullptr;
        uint32_t binding = 0, tex_scale = 0, img_scale = 0;
        for (int s = first; s <= last; s++) {
            if (!src[s].code) continue;
            Shader::IR::Program &prog = programs[s];
            Shader::RuntimeInfo ri = runtime_info(prog, prev, st, s == last_vertex);
            Shader::Maxwell::ConvertLegacyToGeneric(prog, ri);
            std::vector<u32> code = Shader::Backend::SPIRV::EmitSPIRV(profile, ri, prog, bindings);
            out->spirv[s] = static_cast<uint32_t *>(std::malloc(code.size() * 4));
            std::memcpy(out->spirv[s], code.data(), code.size() * 4);
            out->words[s] = code.size();
            out->stages |= 1u << s;
            binding = collect(prog.info, s, binding, descs, tex_scale, img_scale);
            if (prog.info.uses_render_area) out->uses_render_area = 1;
            if (prog.info.uses_rescaling_uniform) out->uses_rescaling = 1;
            if (s == NXL_SH_VS)
                for (int i = 0; i < 32; i++) out->vs_reads_attr[i] = prog.info.loads.Generic(i);
            if (s == NXL_SH_FS) {
                for (int i = 0; i < 8; i++) out->fs_writes[i] = prog.info.stores_frag_color[i];
                out->writes_depth = prog.info.stores_frag_depth;
            }
            if (s == NXL_SH_CS) {
                const auto ws = envs[s]->WorkgroupSize();
                out->local[0] = ws[0], out->local[1] = ws[1], out->local[2] = ws[2];
                out->shared_bytes = envs[s]->SharedMemorySize();
            }
            prev = &prog;
        }
        if (binding != bindings.unified)
            std::snprintf(out->err, sizeof out->err, "binding count mismatch: %u collected, %u emitted", binding, bindings.unified);
    } catch (const std::exception &ex) {
        std::snprintf(out->err, sizeof out->err, "%s", ex.what());
        nxl_sh_free(out);
        return -1;
    }
    out->descs = static_cast<nxl_sh_desc *>(std::malloc((descs.size() + 1) * sizeof(nxl_sh_desc)));
    std::copy(descs.begin(), descs.end(), out->descs);
    out->ndescs = (uint32_t)descs.size();
    out->reads = static_cast<nxl_sh_read *>(std::malloc((reads.size() + 1) * sizeof(nxl_sh_read)));
    std::copy(reads.begin(), reads.end(), out->reads);
    out->nreads = (uint32_t)reads.size();
    return out->err[0] ? -1 : 0;
}

extern "C" uint32_t nxl_sh_desc_handle(const nxl_sh_desc *d, uint32_t i, const nxl_sh_env *env) {
    auto rd = [&](uint32_t index, uint32_t offset) { return env && env->cbuf ? env->cbuf(env->ud, d->stage, index, offset) : 0u; };
    const uint32_t off = d->cbuf_offset + (i << d->size_shift);
    uint32_t lhs = (rd(d->cbuf_index, off) >> d->shift_right) & d->mask;
    if (d->indirect) lhs = rd(d->table_cbuf_index, lhs * d->table_scale + d->table_base) & 0xFFFF;
    if (!d->has_secondary) return lhs;
    const uint32_t rhs = (rd(d->secondary_cbuf_index, d->secondary_cbuf_offset) >> d->secondary_shift_right) & d->secondary_mask;
    return (lhs << d->shift_left) | (rhs << d->secondary_shift_left);
}
