// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMFillFragmentShader(wrapped, interlaced).
//
// Single SPIR-V blob; the original four source-level variants (wrapped x
// interlaced) plus the PGXP_DEPTH constructor-level toggle collapse into
// three boolean specialisation constants. One source -> four (or eight,
// if you count PGXP_DEPTH) specialised pipelines.
//
// Spec constants used by this shader:
//   constant_id =   3  PGXP_DEPTH (bool, common-knob convention).
//   constant_id = 100  INTERLACED (bool, shader-specific).
//   constant_id = 101  WRAPPED    (bool, shader-specific).
//
// Note on cross-shader id reuse: 100 is also used by
// adaptive_downsample_mip.frag.glsl (for FIRST_PASS). Spec-constant ids
// are scoped per-stage in the SPIR-V spec, so reuse across different
// shaders is harmless; each file documents its own ids at the top.
//
// No texture sampling, no descriptor sets - the pipeline layout used by
// this pipeline (m_no_samplers_pipeline_layout) declares only the push
// constant block below.

#version 450 core

layout(constant_id =   3) const bool PGXP_DEPTH = false;
layout(constant_id = 100) const bool INTERLACED = false;
layout(constant_id = 101) const bool WRAPPED    = false;

layout(push_constant) uniform PushConstants {
  uvec2 u_dst_coords;
  uvec2 u_end_coords;
  vec4  u_fill_color;
  uint  u_interlaced_displayed_field;
};

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

void main()
{
  // dst_coords is only needed by the INTERLACED / WRAPPED branches. The
  // driver folds away the uvec2 conversion at pipeline-create time when
  // both spec constants are false.
  uvec2 dst_coords = uvec2(gl_FragCoord.x, gl_FragCoord.y);

  if (INTERLACED)
  {
    if ((dst_coords.y & 1u) == u_interlaced_displayed_field)
      discard;
  }

  if (WRAPPED)
  {
    // Ensure the fragment is inside the (possibly wrapped) destination
    // rectangle; discard otherwise.
    if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
        (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
    {
      discard;
    }
  }

  o_col0 = u_fill_color;
  gl_FragDepth = PGXP_DEPTH ? 1.0 : u_fill_color.a;
}
