// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(use_ssbo=true).
//
// SSBO variant of VRAM write. The host packs PSX 16-bit VRAM data into a
// std430 storage buffer (two 16-bit halves per uint32_t element); this
// shader writes one upscaled pixel per fragment by indexing into that
// buffer and unpacking the appropriate half.
//
// The SSBO vs uniform-texel-buffer split is a STRUCTURAL variant: the
// descriptor type at binding 0 differs (STORAGE_BUFFER vs
// UNIFORM_TEXEL_BUFFER), so a spec constant cannot collapse it. Two
// separate SPIR-V blobs; the C++ side selects based on the per-session
// m_use_ssbos_for_vram_writes runtime decision.
//
// Spec constants used:
//   constant_id = 0  RESOLUTION_SCALE (uint, common-knob convention).
//                    Drives both the upscale-aware coord conversion and
//                    the VRAM_SIZE wrap-around math.
//   constant_id = 3  PGXP_DEPTH       (bool, common-knob convention).
//                    Selects between sampled-alpha-driven depth and a
//                    fixed 1.0.
//
// Bindings (match m_vram_write_pipeline_layout when
// m_use_ssbos_for_vram_writes is true):
//   push_constant     - the UBO block below
//   set=0 binding=0   - readonly restrict SSBO ssbo_data[uint]

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

layout(std430, set = 0, binding = 0) readonly restrict buffer SSBO {
  uint ssbo_data[];
};

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

  // Downscale upscaled fragment coords to PSX-native VRAM coords. The
  // Vulkan fixYCoord is a no-op (PSX VRAM origin matches Vulkan), so we
  // skip the helper entirely.
  uvec2 coords = uvec2(uint(gl_FragCoord.x) / RESOLUTION_SCALE,
                       uint(gl_FragCoord.y) / RESOLUTION_SCALE);

  // Discard fragments outside the (possibly wrap-around) destination rect.
  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||
      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))
  {
    discard;
  }

  // Walk from the start of the destination rect, accounting for wrap.
  uvec2 offset;
  offset.x = (coords.x < u_base_coords.x)
             ? (native_size.x - u_base_coords.x + coords.x)
             : (coords.x - u_base_coords.x);
  offset.y = (coords.y < u_base_coords.y)
             ? (native_size.y - u_base_coords.y + coords.y)
             : (coords.y - u_base_coords.y);

  // Two PSX 16-bit pixels are packed per ssbo_data element; pick the right
  // half via the LSB of the offset.
  uint buffer_offset = u_buffer_base_offset + (offset.y * u_size.x) + offset.x;
  uint packed        = ssbo_data[buffer_offset / 2u];
  uint shift         = (buffer_offset % 2u) * 16u;
  uint value         = (packed >> shift) | u_mask_or_bits;

  o_col0 = RGBA5551ToRGBA8(value);

  gl_FragDepth = PGXP_DEPTH ? 1.0
                            : ((o_col0.a == 1.0) ? u_depth_value : 0.0);
}
