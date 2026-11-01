// SPDX-License-Identifier: GPL-3.0-or-later
// The window's pop-ups (gfx/vk_overlay.c): a quad over the frame, from push constants (its corners in clip space)
#version 450
layout(push_constant) uniform P { vec4 rect; float alpha; } p;
layout(location = 0) out vec2 uv;
void main() {
    vec2 c = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    uv = c;
    gl_Position = vec4(mix(p.rect.xy, p.rect.zw, c), 0.0, 1.0);
}
