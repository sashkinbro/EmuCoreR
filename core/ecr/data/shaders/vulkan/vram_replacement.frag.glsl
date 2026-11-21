// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450 core

layout(push_constant) uniform PushConstants { vec4 u_src_rect; };
layout(set = 0, binding = 1) uniform sampler2D samp0;
layout(location = 0) in VertexData { vec2 v_tex0; };
layout(location = 0) out vec4 o_col0;

void main()
{
  vec2 half_texel = 0.5 / vec2(textureSize(samp0, 0));
  vec2 coords = clamp(u_src_rect.xy + v_tex0 * u_src_rect.zw,
                      u_src_rect.xy + half_texel, u_src_rect.xy + u_src_rect.zw - half_texel);
  o_col0 = texture(samp0, coords);
}
