// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateDisplayFragmentShader(depth_24bit,
//                                                 interlace_mode,
//                                                 smooth_chroma)
// when the source m_vram_texture is single-sample (sampler2D).
//
// The display FS is the densest shader in the project: original
// shadergen takes up to 2 (depth_24bit) x 3 (interlace_mode) x 2
// (smooth_chroma) source variants plus MSAA on top. Two blobs handle
// the structural MSAA split (sampler2D vs sampler2DMS); the remaining
// 12 combinations collapse into five specialisation constants on each
// blob. The C++ side keeps the existing
// m_display_pipelines[depth_24][interlace_mode] 2x3 slot table and
// passes the rest of the knobs as spec constants at pipeline-create
// time. smooth_chroma and resolution scale are per-session-constant
// (UpdateSettings tears the pipelines down on change).
//
// Spec constants used by this shader:
//   constant_id =   0  RESOLUTION_SCALE (uint, common-knob convention).
//                      Drives VRAM_SIZE for the 16bpp clamp/wrap path.
//   constant_id = 100  DEPTH_24BIT      (bool, shader-specific).
//                      Switches between 16bpp direct sampling and
//                      24bpp byte-packed reconstruction.
//   constant_id = 101  INTERLACED       (bool, shader-specific).
//                      Enables field-discard logic for interlaced
//                      display output.
//   constant_id = 102  INTERLEAVED      (bool, shader-specific). Only
//                      meaningful when INTERLACED is true. Selects
//                      "interleaved both fields" vs "single field per
//                      frame" coord folding.
//   constant_id = 103  SMOOTH_CHROMA    (bool, shader-specific). Only
//                      meaningful when DEPTH_24BIT is true. Enables
//                      bilinear UV smoothing in the YUV-space chroma
//                      reconstruction.
//
// Bindings (match m_single_sampler_pipeline_layout):
//   push_constant     - the UBO block below
//   set=0 binding=1   - uniform sampler2D samp0 (the upscaled
//                       m_vram_texture's view)

#version 450 core

layout(constant_id =   0) const uint RESOLUTION_SCALE = 1u;
layout(constant_id = 100) const bool DEPTH_24BIT      = false;
layout(constant_id = 101) const bool INTERLACED       = false;
layout(constant_id = 102) const bool INTERLEAVED      = false;
layout(constant_id = 103) const bool SMOOTH_CHROMA    = false;

layout(push_constant) uniform PushConstants {
  uvec2 u_vram_offset;
  uint  u_crop_left;
  uint  u_field_offset;
};

layout(set = 0, binding = 1) uniform sampler2D samp0;

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

vec3 RGBToYUV(vec3 rgb)
{
  return vec3(dot(rgb, vec3( 0.299,    0.587,    0.114)),
              dot(rgb, vec3(-0.14713, -0.28886,  0.436)),
              dot(rgb, vec3( 0.615,   -0.51499, -0.10001)));
}

vec3 YUVToRGB(vec3 yuv)
{
  return vec3(dot(yuv, vec3(1.0,  0.0,      1.13983)),
              dot(yuv, vec3(1.0, -0.39465, -0.58060)),
              dot(yuv, vec3(1.0,  2.03211,  0.0)));
}

vec4 LoadVRAM(ivec2 coords)
{
  return texelFetch(samp0, coords, 0);
}

uint RGBA8ToRGBA5551(vec4 v)
{
  uint r = uint(roundEven(v.r * 31.0));
  uint g = uint(roundEven(v.g * 31.0));
  uint b = uint(roundEven(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return r | (g << 5) | (b << 10) | (a << 15);
}

vec3 SampleVRAM24(uvec2 icoords)
{
  uvec2 clamp_size = uvec2(1024u, 512u);
  uvec2 vram_coords = u_vram_offset + uvec2((icoords.x * 3u) / 2u, icoords.y);
  uint s0 = RGBA8ToRGBA5551(LoadVRAM(ivec2((vram_coords                  % clamp_size) * RESOLUTION_SCALE)));
  uint s1 = RGBA8ToRGBA5551(LoadVRAM(ivec2(((vram_coords + uvec2(1u, 0u)) % clamp_size) * RESOLUTION_SCALE)));

  uint s1s0 = ((s1 << 16u) | s0) >> ((icoords.x & 1u) * 8u);

  return vec3(float( s1s0         & 0xFFu) / 255.0,
              float((s1s0 >>  8u) & 0xFFu) / 255.0,
              float((s1s0 >> 16u) & 0xFFu) / 255.0);
}

vec3 SampleVRAMAverage2x2(uvec2 icoords)
{
  vec3 value = SampleVRAM24(icoords);
  value     += SampleVRAM24(icoords + uvec2(0u, 1u));
  value     += SampleVRAM24(icoords + uvec2(1u, 0u));
  value     += SampleVRAM24(icoords + uvec2(1u, 1u));
  return value * 0.25;
}

vec3 SampleVRAM24Smoothed(uvec2 icoords)
{
  ivec2 base  = ivec2(icoords) - 1;
  uvec2 low   = uvec2(max(base & ~1, ivec2(0, 0)));
  uvec2 high  = low + 2u;
  vec2  coeff = vec2(base & 1) * 0.5 + 0.25;

  vec3 p   = SampleVRAM24(icoords);
  vec3 p00 = SampleVRAMAverage2x2(low);
  vec3 p01 = SampleVRAMAverage2x2(uvec2(low.x,  high.y));
  vec3 p10 = SampleVRAMAverage2x2(uvec2(high.x, low.y));
  vec3 p11 = SampleVRAMAverage2x2(high);

  vec3 s = mix(mix(p00, p10, coeff.x),
               mix(p01, p11, coeff.x),
               coeff.y);

  float y  = RGBToYUV(p).x;
  vec2  uv = RGBToYUV(s).yz;
  return YUVToRGB(vec3(y, uv));
}

void main()
{
  uvec2 VRAM_SIZE = uvec2(1024u, 512u) * RESOLUTION_SCALE;
  uvec2 icoords   = uvec2(gl_FragCoord.xy) + uvec2(u_crop_left, 0u);

  if (INTERLACED)
  {
    if ((icoords.y & 1u) != u_field_offset)
      discard;

    if (INTERLEAVED)
      icoords.y &= ~1u;
    else
      icoords.y /= 2u;
  }

  if (DEPTH_24BIT)
  {
    if (SMOOTH_CHROMA)
      o_col0 = vec4(SampleVRAM24Smoothed(icoords), 1.0);
    else
      o_col0 = vec4(SampleVRAM24(icoords), 1.0);
  }
  else
  {
    o_col0 = vec4(LoadVRAM(ivec2((icoords + u_vram_offset) % VRAM_SIZE)).rgb, 1.0);
  }
}
