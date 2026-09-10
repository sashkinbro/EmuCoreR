// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(use_ssbo=false).
//
// Uniform-texel-buffer variant of VRAM write. Used on drivers / device
// configurations where the SSBO path is unavailable or undesirable; the
// host packs the same PSX 16-bit VRAM data into a buffer view of format
// R16_UINT, and we read it via samplerBuffer texelFetch. One 16-bit
// value per buffer element.
//
// See vram_write_ssbo.frag.glsl for the structural-variant rationale -
// the descriptor type at binding 0 differs from the SSBO variant
// (UNIFORM_TEXEL_BUFFER vs STORAGE_BUFFER), so two SPIR-V blobs are
// required.
//
// Spec constants (same as the SSBO variant):
//   constant_id = 0  RESOLUTION_SCALE (uint, common-knob convention).
//   constant_id = 3  PGXP_DEPTH       (bool, common-knob convention).
//
// Bindings (match m_vram_write_pipeline_layout when
// m_use_ssbos_for_vram_writes is false):
//   push_constant     - the UBO block below
//   set=0 binding=0   - uniform usamplerBuffer samp0 (R16_UINT view)

#version 450 core

layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;
layout(constant_id = 3) const bool PGXP_DEPTH       = false;

layout(push_constant) uniform PushConstants {
  uvec2 u_base_coords;
  uvec2 u_end_coords;
  uvec2 u_size;
  uint  u_buffer_base_offset;
  uint  u_mask_or_bits;
  float u_depth_value;
};

layout(set = 0, binding = 0) uniform usamplerBuffer samp0;

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

vec4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v       ) & 31u;
  uint g = (v >>  5u) & 31u;
  uint b = (v >> 10u) & 31u;
  uint a = (v >> 15u) &  1u;
  return vec4(float(r) / 31.0, float(g) / 31.0, float(b) / 31.0, float(a));
}

void main()
{
  uvec2 VRAM_SIZE   = uvec2(1024u, 512u) * RESOLUTION_SCALE;
  uvec2 native_size = VRAM_SIZE / RESOLUTION_SCALE;

  uvec2 coords = uvec2(uint(gl_FragCoord.x) / RESOLUTION_SCALE,
                       uint(gl_FragCoord.y) / RESOLUTION_SCALE);

  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||
      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))
  {
    discard;
  }

  uvec2 offset;
  offset.x = (coords.x < u_base_coords.x)
             ? (native_size.x - u_base_coords.x + coords.x)
             : (coords.x - u_base_coords.x);
  offset.y = (coords.y < u_base_coords.y)
             ? (native_size.y - u_base_coords.y + coords.y)
             : (coords.y - u_base_coords.y);

  // One 16-bit value per buffer element (R16_UINT format).
  uint buffer_offset = u_buffer_base_offset + (offset.y * u_size.x) + offset.x;
  uint value         = texelFetch(samp0, int(buffer_offset)).r | u_mask_or_bits;

  o_col0 = RGBA5551ToRGBA8(value);

  gl_FragDepth = PGXP_DEPTH ? 1.0
                            : ((o_col0.a == 1.0) ? u_depth_value : 0.0);
}
