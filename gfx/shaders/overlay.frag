// SPDX-License-Identifier: GPL-3.0-or-later
// The window's pop-ups (gfx/vk_overlay.c): the picture (alpha premultiplied), faded
#version 450
layout(push_constant) uniform P { vec4 rect; float alpha; } p;
layout(set = 0, binding = 0) uniform sampler2D t;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main() { o = texture(t, uv) * p.alpha; }
