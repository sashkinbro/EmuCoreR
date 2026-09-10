// Pre-baked batch fragment shader (textured + JINC2 filter
// family) for the D3D backends.
//
// Equivalent to GPU_HW_ShaderGen::GenerateBatchFragmentShader
// invoked with texture_mode != GPUTextureMode::Disabled AND
// m_texture_filter in {JINC2, JINC2BinAlpha}. The two filter
// enum values share a single HLSL source with a BINALPHA -D
// macro gating the alpha quantisation step at the end of
// FilteredSampleFromVRAM. Mirror of the Vulkan template at
// data/shaders/vulkan/batch_textured_jinc2.frag.glsl in HLSL.
//
// Fourth batch FS template to be pre-baked, after batch_untextured
// (9e6f933 + b6d1903 + c01f8ae), batch_textured_nearest
// (9e4c33d + a7f5717), and batch_textured_bilinear (269a2a0 +
// f2ae92a). One textured filter template remains (xBR / xBRBinAlpha)
// after this one; once it lands the shadergen + D3DCompile
// fallback arm in GetBatchPipeline / GetBatchPixelShader becomes
// unreachable. The "(m_texture_filter == JINC2 ||
// m_texture_filter == JINC2BinAlpha)" gate on the C++ side picks
// this template; xBR still goes through shadergen + D3DCompile
// until its pre-bake commits land.
//
// Variant axes (5, all `-D` to fxc; post-c532a34 TRANSPARENCY
// routing):
//
//   * Texture mode (6 combos, 3 `-D` macros): same encoding as
//     the textured Nearest / Bilinear templates - see
//     batch_textured_nearest.ps.hlsl for the (PALETTE_4_BIT,
//     PALETTE_8_BIT, RAW_TEXTURE) -> (Palette4Bit / Palette8Bit /
//     Direct16Bit each in non-raw / raw form) mapping.
//     PALETTE_4_BIT and PALETTE_8_BIT are mutually exclusive.
//
//   * USE_DUAL_SOURCE (0/1): drives o_col1 declaration + write.
//     For JINC2-family filters, use_dual_source is true whenever
//     m_supports_dual_source_blend is true, since the shadergen
//     formula's `filter != Nearest` clause is always true here.
//
//   * INTERP_CENTROID / INTERP_SAMPLE: input interpolation
//     qualifier (none / centroid / sample tri-state). Same as
//     textured Nearest / Bilinear.
//
//   * NOPERSP (0/1): `noperspective` qualifier on v_col0. Same
//     as textured Nearest / Bilinear.
//
//   * BINALPHA (0/1): gates the ialpha >= 0.5 quantisation step
//     at the end of FilteredSampleFromVRAM. When 0 (JINC2), the
//     resampler-weighted alpha passes through as a smooth value;
//     when 1 (JINC2BinAlpha), it's quantised to {0, 1} before
//     the body's `ialpha < 0.5 ? discard : ...` test. fxc dead-
//     strips the unused arm at compile time.
//
// 6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) x 2 (BinAlpha)
// = 144 blobs. Same axis cardinality as the Bilinear template;
// only the FilteredSampleFromVRAM body differs (16-tap sinc-
// windowed resampler with anti-ringing vs 4-tap bilinear). The
// per-variant DXBC is larger than Bilinear (~13-15 KiB vs ~6 KiB
// for Bilinear) because JINC2's 16 SampleFromVRAM calls + the
// 4x4 weight matrix expand the bytecode substantially. The MSAA
// axis still does NOT multiply this template - the batch FS
// reads from the single-sample shadow VRAM regardless of
// m_multisamples; MSAA only affects the interp qualifier which
// the INTERP_* axes already cover.
//
// UV_LIMITS is implicit for all non-Nearest filters
// (ShouldUseUVLimits in gpu_hw.cpp forces m_using_uv_limits true
// when the filter is non-Nearest). The shader unconditionally
// clamps to v_uv_limits without a runtime cbuffer gate.
//
// Variant suffix: pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_b{0,1}.
// The `b` digit encodes BINALPHA (0 = JINC2, 1 = JINC2BinAlpha).
// Alphabetical sort matches the natural nested-loop iteration
// order in the matching helper.


#define HLSL 1
#define CONSTANT static const
#define VECTOR_EQ(a, b) (all((a) == (b)))
#define VECTOR_NEQ(a, b) (any((a) != (b)))

cbuffer UBOBlock : register(b0)
{
  // 64-byte cbuffer. Identical layout to the textured Nearest
  // template - same BatchUBOData struct at the C++ side (gpu_hw.h:111).
  // u_render_mode at offset 60 is the post-c532a34 TRANSPARENCY-
  // routed runtime branch driver.
  uint2 u_texture_window_and;
  uint2 u_texture_window_or;
  float u_src_alpha_factor;
  float u_dst_alpha_factor;
  uint  u_interlaced_displayed_field;
  bool  u_set_mask_while_drawing;
  uint  u_resolution_scale;
  uint  u_true_color;
  uint  u_scaled_dithering;
  uint  u_dithering;
  uint  u_interlacing;
  uint  u_pgxp_depth;
  uint  u_uv_limits;
  uint  u_render_mode;
};

Texture2D    samp0    : register(t0);
SamplerState samp0_ss : register(s0);

CONSTANT int s_dither_values[16] = {
  -4, 0, -3, 1,
  2, -2, 3, -1,
  -3, 1, -4, 0,
  3, -1, 2, -2
};

uint3 ApplyDithering(uint2 coord, uint3 icol)
{
  uint2 fc;
  if (u_scaled_dithering != 0u)
    fc = coord & uint2(3u, 3u);
  else
    fc = (coord / uint2(u_resolution_scale, u_resolution_scale)) & uint2(3u, 3u);
  int offset = s_dither_values[fc.y * 4u + fc.x];

  if (u_true_color != 0u)
    return uint3(clamp(int3(icol) + int3(offset, offset, offset), 0, 255));
  else
    return uint3(clamp((int3(icol) + int3(offset, offset, offset)) >> 3, 0, 31));
}

uint2 ApplyTextureWindow(uint2 coords)
{
  uint x = (coords.x & u_texture_window_and.x) | u_texture_window_or.x;
  uint y = (coords.y & u_texture_window_and.y) | u_texture_window_or.y;
  return uint2(x, y);
}

uint2 ApplyUpscaledTextureWindow(uint2 coords)
{
  uint2 native_coords = coords / uint2(u_resolution_scale, u_resolution_scale);
  uint2 coords_offset = coords % uint2(u_resolution_scale, u_resolution_scale);
  return (ApplyTextureWindow(native_coords) * uint2(u_resolution_scale, u_resolution_scale)) + coords_offset;
}

uint2 FloatToIntegerCoords(float2 coords)
{
  return uint2((u_resolution_scale == 1u) ? round(coords) : floor(coords));
}

uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(round(v.r * 31.0));
  uint g = uint(round(v.g * 31.0));
  uint b = uint(round(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return r | (g << 5) | (b << 10) | (a << 15);
}

// Per-texel VRAM sample (no filtering). Identical to the textured
// Nearest template's SampleFromVRAM. The bilinear filter calls
// this 4 times per pixel for the 4 corner taps.
float4 SampleFromVRAM(uint4 texpage, float2 coords)
{
  float2 rcp_vram_size = float2(1.0, 1.0) / float2(uint2(1024u, 512u) * u_resolution_scale);

#if PALETTE_4_BIT || PALETTE_8_BIT
  uint2 icoord = ApplyTextureWindow(FloatToIntegerCoords(coords));
  uint2 index_coord = icoord;
#  if PALETTE_4_BIT
  index_coord.x /= 4u;
#  elif PALETTE_8_BIT
  index_coord.x /= 2u;
#  endif

  uint2 vicoord = uint2(texpage.x + index_coord.x * u_resolution_scale,
                        texpage.y + index_coord.y * u_resolution_scale);
  float4 texel = samp0.Sample(samp0_ss, float2(vicoord) * rcp_vram_size);
  uint vram_value = RGBA8ToRGBA5551(texel);

#  if PALETTE_4_BIT
  uint subpixel = icoord.x & 3u;
  uint palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
#  elif PALETTE_8_BIT
  uint subpixel = icoord.x & 1u;
  uint palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
#  endif

  uint2 palette_icoord = uint2(texpage.z + (palette_index * u_resolution_scale),
                               texpage.w);
  return samp0.Sample(samp0_ss, float2(palette_icoord) * rcp_vram_size);
#else
  uint2 icoord = ApplyUpscaledTextureWindow(FloatToIntegerCoords(coords));
  uint2 direct_icoord = uint2(texpage.x + icoord.x, texpage.y + icoord.y);
  return samp0.Sample(samp0_ss, float2(direct_icoord) * rcp_vram_size);
#endif
}

// JINC2 (sinc-windowed) 4x4 = 16-tap weighted sample, with anti-
// ringing. Body lifted from gpu_hw_shadergen.cpp::WriteBatchTextureFilter
// for the JINC2 / JINC2BinAlpha cases. The BINALPHA -D macro merges
// the two enum values into a single template, with fxc dead-stripping
// the unused arm.
//
// Constants from the Vulkan template:
//   JINC2_AR_STRENGTH (0.8) - anti-ringing lerp strength
//   JINC2_WA          (~1.382) - first sinc kernel frequency
//   JINC2_WB          (~2.576) - second sinc kernel frequency
//
// AR clamps the resampler output to the colour range of the 2x2
// central footprint, then lerps back toward the unclamped value
// at the AR_STRENGTH ratio. This suppresses Gibbs-style ringing at
// hard edges that the sinc kernel would otherwise produce.

CONSTANT float JINC2_AR_STRENGTH = 0.8;
CONSTANT float JINC2_WA          = 1.382300768;
CONSTANT float JINC2_WB          = 2.576105976;

float dist2d(float2 pt1, float2 pt2)
{
  float2 v = pt2 - pt1;
  return sqrt(dot(v, v));
}

float4 jinc2_resampler(float4 x)
{
  // sinc(WA*x) * sinc(WB*x) for x != 0; WA*WB for x == 0.
  // Implemented branchlessly via a per-component equality check
  // and lerp - same shape as the Vulkan template's mix(...).
  float4 sinc_value = sin(x * JINC2_WA) * sin(x * JINC2_WB) / (x * x);
  float4 limit_value = float4(JINC2_WA * JINC2_WB, JINC2_WA * JINC2_WB,
                              JINC2_WA * JINC2_WB, JINC2_WA * JINC2_WB);
  return lerp(sinc_value, limit_value, x == float4(0.0, 0.0, 0.0, 0.0));
}

void FilteredSampleFromVRAM(uint4 texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  float4 weights[4];
  float2 dx = float2(1.0, 0.0);
  float2 dy = float2(0.0, 1.0);
  float2 pc = coords.xy;
  float2 tc = floor(pc - float2(0.5, 0.5)) + float2(0.5, 0.5);

  weights[0] = jinc2_resampler(float4(dist2d(pc, tc       - dx       - dy),
                                      dist2d(pc, tc                  - dy),
                                      dist2d(pc, tc       + dx       - dy),
                                      dist2d(pc, tc + 2.0 * dx       - dy)));
  weights[1] = jinc2_resampler(float4(dist2d(pc, tc       - dx           ),
                                      dist2d(pc, tc                      ),
                                      dist2d(pc, tc       + dx           ),
                                      dist2d(pc, tc + 2.0 * dx           )));
  weights[2] = jinc2_resampler(float4(dist2d(pc, tc       - dx       + dy),
                                      dist2d(pc, tc                  + dy),
                                      dist2d(pc, tc       + dx       + dy),
                                      dist2d(pc, tc + 2.0 * dx       + dy)));
  weights[3] = jinc2_resampler(float4(dist2d(pc, tc       - dx + 2.0 * dy),
                                      dist2d(pc, tc           + 2.0 * dy),
                                      dist2d(pc, tc       + dx + 2.0 * dy),
                                      dist2d(pc, tc + 2.0 * dx + 2.0 * dy)));

  // 16 corner taps. Each is a SampleFromVRAM call clamped to the
  // polygon's UV bounds. Per-tap alpha is 0 for the VRAM "0000h"
  // transparency sentinel and 1 otherwise; this drives both the
  // weighted-coverage ialpha output and the AR clamp range.
#define JS(c) SampleFromVRAM(texpage, clamp((c), uv_limits.xy, uv_limits.zw))
  float4 c00 = JS(tc       - dx       - dy); float a00 = VECTOR_NEQ(c00, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c10 = JS(tc                  - dy); float a10 = VECTOR_NEQ(c10, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c20 = JS(tc       + dx       - dy); float a20 = VECTOR_NEQ(c20, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c30 = JS(tc + 2.0 * dx       - dy); float a30 = VECTOR_NEQ(c30, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c01 = JS(tc       - dx           ); float a01 = VECTOR_NEQ(c01, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c11 = JS(tc                      ); float a11 = VECTOR_NEQ(c11, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c21 = JS(tc       + dx           ); float a21 = VECTOR_NEQ(c21, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c31 = JS(tc + 2.0 * dx           ); float a31 = VECTOR_NEQ(c31, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c02 = JS(tc       - dx       + dy); float a02 = VECTOR_NEQ(c02, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c12 = JS(tc                  + dy); float a12 = VECTOR_NEQ(c12, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c22 = JS(tc       + dx       + dy); float a22 = VECTOR_NEQ(c22, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c32 = JS(tc + 2.0 * dx       + dy); float a32 = VECTOR_NEQ(c32, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c03 = JS(tc       - dx + 2.0 * dy); float a03 = VECTOR_NEQ(c03, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c13 = JS(tc            + 2.0 * dy); float a13 = VECTOR_NEQ(c13, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c23 = JS(tc       + dx + 2.0 * dy); float a23 = VECTOR_NEQ(c23, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 c33 = JS(tc + 2.0 * dx + 2.0 * dy); float a33 = VECTOR_NEQ(c33, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
#undef JS

  // Min/max around the central 2x2 cell for anti-ringing clamp range.
  float4 min_sample = min(min(c11, c21), min(c12, c22));
  float min_sample_alpha = min(min(a11, a21), min(a12, a22));
  float4 max_sample = max(max(c11, c21), max(c12, c22));
  float max_sample_alpha = max(max(a11, a21), max(a12, a22));

  // 16-tap weighted sum over the 4x4 footprint.
  float4 color;
  color  = float4(dot(weights[0], float4(c00.x, c10.x, c20.x, c30.x)),
                  dot(weights[0], float4(c00.y, c10.y, c20.y, c30.y)),
                  dot(weights[0], float4(c00.z, c10.z, c20.z, c30.z)),
                  dot(weights[0], float4(c00.w, c10.w, c20.w, c30.w)));
  color += float4(dot(weights[1], float4(c01.x, c11.x, c21.x, c31.x)),
                  dot(weights[1], float4(c01.y, c11.y, c21.y, c31.y)),
                  dot(weights[1], float4(c01.z, c11.z, c21.z, c31.z)),
                  dot(weights[1], float4(c01.w, c11.w, c21.w, c31.w)));
  color += float4(dot(weights[2], float4(c02.x, c12.x, c22.x, c32.x)),
                  dot(weights[2], float4(c02.y, c12.y, c22.y, c32.y)),
                  dot(weights[2], float4(c02.z, c12.z, c22.z, c32.z)),
                  dot(weights[2], float4(c02.w, c12.w, c22.w, c32.w)));
  color += float4(dot(weights[3], float4(c03.x, c13.x, c23.x, c33.x)),
                  dot(weights[3], float4(c03.y, c13.y, c23.y, c33.y)),
                  dot(weights[3], float4(c03.z, c13.z, c23.z, c33.z)),
                  dot(weights[3], float4(c03.w, c13.w, c23.w, c33.w)));
  float wsum = dot(weights[0], float4(1.0, 1.0, 1.0, 1.0))
             + dot(weights[1], float4(1.0, 1.0, 1.0, 1.0))
             + dot(weights[2], float4(1.0, 1.0, 1.0, 1.0))
             + dot(weights[3], float4(1.0, 1.0, 1.0, 1.0));
  color = color / wsum;

  float alpha;
  alpha  = dot(weights[0], float4(a00, a10, a20, a30));
  alpha += dot(weights[1], float4(a01, a11, a21, a31));
  alpha += dot(weights[2], float4(a02, a12, a22, a32));
  alpha += dot(weights[3], float4(a03, a13, a23, a33));
  alpha = alpha / wsum;

  // Anti-ringing: clamp to the central-2x2 min/max range, then lerp
  // back toward the unclamped resampler output at AR_STRENGTH ratio.
  float4 aux = color;
  float aux_alpha = alpha;
  color = clamp(color, min_sample, max_sample);
  alpha = clamp(alpha, min_sample_alpha, max_sample_alpha);
  color = lerp(aux, color, JINC2_AR_STRENGTH);
  alpha = lerp(aux_alpha, alpha, JINC2_AR_STRENGTH);

  ialpha = alpha;
  texcol = color;

  // Compensate for partially-transparent sampling - divide by ialpha
  // so the visible texels' RGB doesn't get pulled toward zero by
  // their transparent neighbours.
  if (ialpha > 0.0)
    texcol.rgb /= float3(ialpha, ialpha, ialpha);

  // BinAlpha: quantise the resampler-interpolated alpha to {0, 1}.
  // Drops the partial-coverage smooth-alpha behaviour at texture
  // edges; produces sharper alpha cutouts at the cost of stair-
  // stepping.
#if BINALPHA
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}

#if INTERP_SAMPLE
#  define INTERP sample
#elif INTERP_CENTROID
#  define INTERP centroid
#else
#  define INTERP
#endif

#if NOPERSP
#  define COLOR_INTERP noperspective INTERP
#else
#  define COLOR_INTERP INTERP
#endif

void main(
  COLOR_INTERP in float4 v_col0       : COLOR0,
  INTERP       in float2 v_tex0       : TEXCOORD0,
  nointerpolation in uint4 v_texpage  : TEXCOORD1,
  nointerpolation in float4 v_uv_limits : TEXCOORD2,
  in float4 v_pos : SV_Position,
  out float o_depth : SV_Depth,
  out float4 o_col0 : SV_Target0
#if USE_DUAL_SOURCE
  , out float4 o_col1 : SV_Target1
#endif
  )
{
  uint3 vertcol = uint3(v_col0.rgb * float3(255.0, 255.0, 255.0));

  if (u_interlacing != 0u && (uint(v_pos.y) & 1u) == u_interlaced_displayed_field)
    discard;

  // Texture coords. Palette textures index the VRAM atlas with
  // native-resolution coords; direct-16bpp textures use upscaled
  // coords. The /= u_resolution_scale path applies to palette
  // modes only.
  float2 coords = v_tex0;
#if PALETTE_4_BIT || PALETTE_8_BIT
  coords /= float(u_resolution_scale);
#endif

  // UV limits unconditional on filter templates (ShouldUseUVLimits
  // forces m_using_uv_limits true for non-Nearest filters), so no
  // runtime branch on u_uv_limits here. Direct-16bpp extends the
  // UV range to all upscaled pixels per the same reasoning as
  // the textured Nearest template.
  float4 uv_limits = v_uv_limits;
#if !(PALETTE_4_BIT || PALETTE_8_BIT)
  uv_limits = uv_limits * float(u_resolution_scale);
  uv_limits.zw += float(u_resolution_scale - 1u);
#endif

  // Filtered sample replaces the Nearest template's single
  // SampleFromVRAM call. Bilinear returns texcol + per-corner-
  // alpha-weighted ialpha; the body uses ialpha for the discard
  // threshold and as the multiplier for the dual-source
  // u_dst_alpha_factor output.
  float4 texcol;
  float ialpha;
  FilteredSampleFromVRAM(v_texpage, coords, uv_limits, texcol, ialpha);

  // Bilinear discard threshold: drop the pixel if more than half
  // its bilinear-weighted area would have come from transparent
  // texels. Replaces the Nearest template's all-zero-sentinel
  // discard.
  if (ialpha < 0.5)
    discard;

  bool semitransparent = (texcol.a >= 0.5);

  uint3 icolor;
  if (u_true_color == 0u)
  {
    icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0)) >> 3;
#if !RAW_TEXTURE
    icolor = (icolor * vertcol) >> 4;
    if (u_dithering != 0u)
      icolor = ApplyDithering(uint2(v_pos.xy), icolor);
    else
      icolor = min(icolor >> 3, uint3(31u, 31u, 31u));
#endif
  }
  else
  {
    icolor = uint3(texcol.rgb * float3(255.0, 255.0, 255.0));
#if !RAW_TEXTURE
    icolor = (icolor * vertcol) >> 7;
    if (u_dithering != 0u)
      icolor = ApplyDithering(uint2(v_pos.xy), icolor);
    else
      icolor = min(icolor, uint3(255u, 255u, 255u));
#endif
  }

  float oalpha = u_set_mask_while_drawing ? 1.0 : (semitransparent ? 1.0 : 0.0);

  float premultiply_alpha = ialpha;
  if (u_render_mode != 0u)
    premultiply_alpha = ialpha * (semitransparent ? u_src_alpha_factor : 1.0);

  float3 color;
  if (u_true_color != 0u)
  {
    color = (float3(icolor) * premultiply_alpha) / float3(255.0, 255.0, 255.0);
  }
  else
  {
    color = floor(float3(icolor) * premultiply_alpha) / float3(31.0, 31.0, 31.0);
  }

  if (u_render_mode != 0u)
  {
    if (semitransparent)
    {
      o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
#endif
      if (u_render_mode == 2u)
        discard;
    }
    else
    {
      o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif
      if (u_render_mode == 3u)
        discard;
    }
  }
  else
  {
    o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
    o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif
  }

  o_depth = (u_pgxp_depth != 0u) ? v_pos.z : (oalpha * v_pos.z);
}
