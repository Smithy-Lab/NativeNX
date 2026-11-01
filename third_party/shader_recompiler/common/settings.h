// SPDX-License-Identifier: GPL-3.0-or-later
// nxlinux: the few of yuzu's settings the shader recompiler reads, fixed (no resolution scaling, shader loop
// safety checks on, no wider reciprocals)
#pragma once
#include "common/common_types.h"

namespace Settings {
template <typename T> struct Value {
    T v;
    T GetValue() const { return v; }
    operator T() const { return v; }
};
struct ResolutionScalingInfo {
    u32 up_scale{1};
    u32 down_shift{0};
    f32 up_factor{1.0f};
    f32 down_factor{1.0f};
    bool active{};
};
struct Values {
    ResolutionScalingInfo resolution_info;
    Value<bool> renderer_debug{false};
    Value<bool> wider_reciprocals{false};
    Value<bool> disable_shader_loop_safety_checks{false};
};
inline Values values;
} // namespace Settings
