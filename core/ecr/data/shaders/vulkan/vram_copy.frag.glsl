// Pre-baked Vulkan source for GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader.
//
// Copies a rectangular region from one part of upscaled VRAM to another,
// with PSX-style modulo-1024x512-VRAM wrap-around at the destination and
// optional mask-bit writeback. Single SPIR-V blob covers all variants:
//
//   constant_id =   0  RESOLUTION_SCALE (uint, common-knob convention).
//                      Drives VRAM_SIZE in modulo + offset math.
//   constant_id =   3  PGXP_DEPTH (bool, common-knob convention).
//                      When true, depth is fixed at 1.0; otherwise it is
//                      derived from the sampled alpha and u_depth_value.
//
// The shadergen has a hard-coded msaa=false (see the TODO at the top of
// GenerateVRAMCopyFragmentShader in src/core/gpu_hw_shadergen.cpp: the
// source texture can't be bound as both a colour attachment and a sampler
// at the same time, so VRAM copy never goes through the multisample
// sampling path). No MSAA blob needed.
//
// Bindings (match m_single_sampler_pipeline_layout):
//   push_constant  - the UBO block below
//   set=0 binding=1 - sampler2D samp0  (source texture, single-sample)
//
// Pipeline-state knobs unrelated to the shader source itself:
//   - depth_test toggle drives SetDepthState at pipeline-create time. The
//     C++ caller maintains a 2-entry pipeline cache keyed by depth_test.

#version 450 core

layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;
layout(constant_id = 3) const bool PGXP_DEPTH       = false;

layout(push_constant) uniform PushConstants {
  uvec2 u_src_coords;
  uvec2 u_dst_coords;
  uvec2 u_end_coords;
  uvec2 u_size;
  bool  u_set_mask_bit;
  float u_depth_value;
};

layout(set = 0, binding = 1) uniform sampler2D samp0;

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

void main()
{
  uvec2 VRAM_SIZE = uvec2(1024u, 512u) * RESOLUTION_SCALE;
  uvec2 dst_coords = uvec2(gl_FragCoord.x, gl_FragCoord.y);

  // Discard fragments outside the (possibly wrap-around) destination rect.
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }

  // Walk from the start of the destination rect, accounting for wrap.
  uvec2 offset;
  offset.x = (dst_coords.x < u_dst_coords.x)
             ? (VRAM_SIZE.x - u_dst_coords.x + dst_coords.x)
             : (dst_coords.x - u_dst_coords.x);
  offset.y = (dst_coords.y < u_dst_coords.y)
             ? (VRAM_SIZE.y - u_dst_coords.y + dst_coords.y)
             : (dst_coords.y - u_dst_coords.y);

  uvec2 src_coords = (u_src_coords + offset) % VRAM_SIZE;

  vec4 color = texelFetch(samp0, ivec2(src_coords), 0);
  o_col0     = vec4(color.xyz, u_set_mask_bit ? 1.0 : color.a);

  gl_FragDepth = PGXP_DEPTH
                 ? 1.0
                 : (u_set_mask_bit ? 1.0
                                   : ((o_col0.a == 1.0) ? u_depth_value : 0.0));
}
