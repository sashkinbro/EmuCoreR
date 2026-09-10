// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateDisplayFragmentShader(depth_24bit,
//                                                 interlace_mode,
//                                                 smooth_chroma)
// when the source m_vram_texture is multisampled (sampler2DMS).
//
// See display.frag.glsl for the non-MSAA blob and the full rationale
// for the two-blob structural split. This file is identical to its
// non-MSAA twin in spec-constant interface and main-body logic; the
// only differences are:
//
//   - samp0 is declared sampler2DMS instead of sampler2D.
//   - LoadVRAM averages MULTISAMPLES texelFetch results instead of
//     reading a single sample. The loop bound is a specialisation
//     constant, so the driver unrolls and folds it at pipeline-create
//     time when it specialises this blob for a concrete sample count.
//   - An additional spec constant at constant_id=1 carries the
//     MULTISAMPLES value (matches the project-wide convention used by
//     the VRAM read / update-depth MSAA blobs).
//
// Spec constants used by this shader (a strict superset of the
// non-MSAA blob's set):
//   constant_id =   0  RESOLUTION_SCALE (uint, common-knob convention).
//   constant_id =   1  MULTISAMPLES     (uint, common-knob convention).
//                      Drives the LoadVRAM averaging loop bound.
//   constant_id = 100  DEPTH_24BIT      (bool, shader-specific).
//   constant_id = 101  INTERLACED       (bool, shader-specific).
//   constant_id = 102  INTERLEAVED      (bool, shader-specific).
//   constant_id = 103  SMOOTH_CHROMA    (bool, shader-specific).
//
// Bindings (match m_single_sampler_pipeline_layout):
//   push_constant     - the UBO block below
//   set=0 binding=1   - uniform sampler2DMS samp0 (the upscaled
//                       m_vram_texture's view in MSAA mode)

#version 450 core

layout(constant_id =   0) const uint RESOLUTION_SCALE = 1u;
layout(constant_id =   1) const uint MULTISAMPLES     = 1u;
layout(constant_id = 100) const bool DEPTH_24BIT      = false;
layout(constant_id = 101) const bool INTERLACED       = false;
layout(constant_id = 102) const bool INTERLEAVED      = false;
layout(constant_id = 103) const bool SMOOTH_CHROMA    = false;

layout(push_constant) uniform PushConstants {
  uvec2 u_vram_offset;
  uint  u_crop_left;
  uint  u_field_offset;
};

layout(set = 0, binding = 1) uniform sampler2DMS samp0;

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
  vec4 value = texelFetch(samp0, coords, 0);
  for (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += texelFetch(samp0, coords, int(sample_index));
  return value / float(MULTISAMPLES);
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
