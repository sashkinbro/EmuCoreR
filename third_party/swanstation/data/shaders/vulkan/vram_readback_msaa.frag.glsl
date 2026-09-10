// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader() when the source
// m_vram_texture is multisampled (sampler2DMS).
//
// See vram_readback.frag.glsl for the non-MSAA blob and the full
// rationale. This MSAA twin only differs in:
//   - samp0 is declared sampler2DMS.
//   - LoadVRAM averages MULTISAMPLES texelFetch results.
//
// The box-filter SampleVRAM logic over RESOLUTION_SCALE is identical;
// the per-call LoadVRAM averaging is what makes this an MSAA blob.
//
// Spec constants used by this shader:
//   constant_id = 0  RESOLUTION_SCALE (uint, common-knob convention).
//   constant_id = 1  MULTISAMPLES     (uint, common-knob convention).
//
// Bindings (match m_single_sampler_pipeline_layout):
//   push_constant     - the UBO block below
//   set=0 binding=1   - uniform sampler2DMS samp0 (m_vram_texture view)

#version 450 core

layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;
layout(constant_id = 1) const uint MULTISAMPLES     = 1u;

layout(push_constant) uniform PushConstants {
  uvec2 u_base_coords;
  uvec2 u_size;
};

layout(set = 0, binding = 1) uniform sampler2DMS samp0;

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

uint RGBA8ToRGBA5551(vec4 v)
{
  uint r = uint(roundEven(v.r * 31.0));
  uint g = uint(roundEven(v.g * 31.0));
  uint b = uint(roundEven(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return r | (g << 5) | (b << 10) | (a << 15);
}

vec4 LoadVRAM(ivec2 coords)
{
  vec4 value = texelFetch(samp0, coords, 0);
  for (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += texelFetch(samp0, coords, int(sample_index));
  return value / float(MULTISAMPLES);
}

uint SampleVRAM(uvec2 coords)
{
  if (RESOLUTION_SCALE == 1u)
    return RGBA8ToRGBA5551(LoadVRAM(ivec2(coords)));

  vec4  value       = vec4(0.0);
  uvec2 base_coords = coords * RESOLUTION_SCALE;
  for (uint oy = 0u; oy < RESOLUTION_SCALE; oy++)
    for (uint ox = 0u; ox < RESOLUTION_SCALE; ox++)
      value += LoadVRAM(ivec2(base_coords + uvec2(ox, oy)));
  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  return RGBA8ToRGBA5551(value);
}

void main()
{
  uvec2 sample_coords = uvec2(uint(gl_FragCoord.x) * 2u,
                              uint(gl_FragCoord.y));
  sample_coords += u_base_coords;

  uint left  = SampleVRAM(sample_coords);
  uint right = SampleVRAM(uvec2(sample_coords.x + 1u, sample_coords.y));

  o_col0 = vec4(float( left         & 0xFFu),
                float((left  >> 8u) & 0xFFu),
                float( right        & 0xFFu),
                float((right >> 8u) & 0xFFu)) / 255.0;
}
