// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader() when the source
// m_vram_texture is single-sample (sampler2D).
//
// "VRAM read" is the misleading shadergen name. The pipeline this
// shader belongs to (m_vram_readback_pipeline in gpu_hw_vulkan.cpp) is
// CPU readback: it samples the upscaled m_vram_texture, downsamples
// blocks back to PSX-native 16bpp, and packs two adjacent 16-bit
// pixels into one RGBA8 output texel. The host then transfers that
// output texture to staging and reads it on the CPU.
//
// Two blobs handle the structural MSAA split (sampler2D vs sampler2DMS,
// matching the display FS pattern); RESOLUTION_SCALE folds into a spec
// constant, so a single blob covers all upscale factors. The OpenGL
// "lower-left origin flip" branch in the shadergen source is dropped
// because Vulkan VRAM Y matches scanout Y directly.
//
// Spec constants used by this shader:
//   constant_id = 0  RESOLUTION_SCALE (uint, common-knob convention).
//                    Drives both the base_coords stride and the
//                    box-filter inner-loop bounds.
//
// Bindings (match m_single_sampler_pipeline_layout):
//   push_constant     - the UBO block below
//   set=0 binding=1   - uniform sampler2D samp0 (m_vram_texture view)

#version 450 core

layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;

layout(push_constant) uniform PushConstants {
  uvec2 u_base_coords;
  uvec2 u_size;
};

layout(set = 0, binding = 1) uniform sampler2D samp0;

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
  return texelFetch(samp0, coords, 0);
}

uint SampleVRAM(uvec2 coords)
{
  if (RESOLUTION_SCALE == 1u)
    return RGBA8ToRGBA5551(LoadVRAM(ivec2(coords)));

  // Box filter for downsampling upscaled VRAM back to native.
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

  // Output is 32-bit-per-texel; two PSX 16bpp pixels packed per output.
  uint left  = SampleVRAM(sample_coords);
  uint right = SampleVRAM(uvec2(sample_coords.x + 1u, sample_coords.y));

  o_col0 = vec4(float( left         & 0xFFu),
                float((left  >> 8u) & 0xFFu),
                float( right        & 0xFFu),
                float((right >> 8u) & 0xFFu)) / 255.0;
}
