// Pre-baked batch fragment shader (textured + Nearest-filter slice)
// for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateBatchFragmentShader invoked
// with texture_mode != GPUTextureMode::Disabled AND m_texture_filter
// == GPUTextureFilter::Nearest. Mirror of the Vulkan template at
// data/shaders/vulkan/batch_textured_nearest.frag.glsl in HLSL.
//
// Second batch FS template to be pre-baked, after batch_untextured.
// The remaining three textured filter templates (Bilinear, JINC2,
// xBR - each with a *BinAlpha variant baked together) land in
// subsequent commits. The "m_texture_filter == Nearest" gate on the
// C++ side in GetBatchFragmentShader picks this template; other
// filter values still go through shadergen + D3DCompile until their
// pre-bake commits land.
//
// Variant axes (6, all `-D` to fxc; post-c532a34 TRANSPARENCY routing):
//
//   * Texture mode (6 combos, 3 `-D` macros):
//       PALETTE_4_BIT (0/1), PALETTE_8_BIT (0/1), RAW_TEXTURE (0/1)
//     Valid (P4, P8, RAW) combinations:
//       (1, 0, 0): Palette4Bit
//       (0, 1, 0): Palette8Bit
//       (0, 0, 0): Direct16Bit
//       (1, 0, 1): RawPalette4Bit
//       (0, 1, 1): RawPalette8Bit
//       (0, 0, 1): RawDirect16Bit
//     PALETTE_4_BIT and PALETTE_8_BIT are mutually exclusive; we
//     never emit a variant where both are 1. The Reserved_Direct16Bit
//     / Reserved_RawDirect16Bit texture_mode enum values fold into
//     Direct16Bit / RawDirect16Bit at the C++ picker level (their
//     shader output is identical).
//
//   * USE_DUAL_SOURCE (0/1): drives o_col1 declaration + write.
//     Per-call value derived in C++ from
//     m_supports_dual_source_blend AND ((render_mode !=
//     TransparencyDisabled && render_mode != OnlyOpaque) ||
//     m_texture_filter != Nearest). For this Nearest template the
//     filter clause is false, so dual_source toggles purely on the
//     transparency-mode + dual-source-support combo.
//
//   * INTERP_CENTROID / INTERP_SAMPLE: input interpolation
//     qualifier. Mutually exclusive booleans encoding the (none /
//     centroid / sample) tri-state. INTERP_SAMPLE wins if both are
//     set. Maps to ShaderGen::GetInterpolationQualifier with msaa /
//     per_sample_shading derived from m_multisamples > 1 and
//     m_per_sample_shading respectively.
//
//   * NOPERSP (0/1): `noperspective` qualifier on v_col0. Set when
//     m_disable_color_perspective is true (PGXP texture correction
//     enabled without colour correction - niche).
//
// 6 (texture_mode) x 2 (dual) x 3 (interp) x 2 (persp) = 72 blobs.
// The MSAA axis (m_multisamples 1/2/4/8/16/32) does NOT multiply
// this template - the batch FS samples from a single-sample shadow
// VRAM texture regardless of m_multisamples, so there's no
// LOAD_TEXTURE_MS sample-resolve loop to unroll. MSAA only affects
// the interpolation qualifier (centroid/sample), which the INTERP_*
// axes already cover.
//
// Variant suffix: {p4r0,p8r0,p0r0,p4r1,p8r1,p0r1}_d{0,1}_{none,centroid,sample}_n{0,1}.
// The `pX` prefix encodes the palette mode (4-bit / 8-bit / none);
// `rY` encodes RawTextureBit; `dZ` encodes USE_DUAL_SOURCE; the
// interp word is the qualifier; `nW` encodes NOPERSP. Alphabetical
// sort matches the natural nested-loop iteration order in the
// matching helper at gpu_hw_d3d12.cpp.

#define HLSL 1
#define CONSTANT static const
#define VECTOR_EQ(a, b) (all((a) == (b)))

cbuffer UBOBlock : register(b0)
{
  // 64-byte cbuffer. C++ side is GPU_HW::BatchUBOData (gpu_hw.h:111);
  // field order MUST match the struct member order there.
  //
  // u_render_mode at offset 60 (former u_pad2 pre-c532a34) is read
  // at the premultiply + output-arm sites as a runtime branch on
  // the 4-state BatchRenderMode enum. u_uv_limits drives whether to
  // consume v_uv_limits for clamping at the texture-sample site.
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

// VRAM atlas sampler. Single-sample regardless of m_multisamples -
// the batch FS reads from the shadow VRAM, not from the multisampled
// render target. m_batch_root_signature binds Texture2D at register
// t0 + SamplerState at register s0 (see CreateRootSignatures in
// gpu_hw_d3d12.cpp).
Texture2D    samp0    : register(t0);
SamplerState samp0_ss : register(s0);

// PSX dither matrix. Constant on every variant - the same 4x4 spread
// of [-4..3] values the original PSX hardware applies before the
// 5-bit-per-channel framebuffer truncation. ApplyDithering looks up
// per-pixel via (v_pos.xy mod 4) when u_dithering != 0.
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

// Texture-window helpers. Mirror of the shadergen
// ApplyTextureWindow / ApplyUpscaledTextureWindow at line 887+ of
// gpu_hw_shadergen.cpp.
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
  // 1x-resolution-scale: round to nearest texel (the +0.5 vertex
  // offset from the batch VS aligned us on a texel centre). >1x:
  // floor, since the vertex offset is pre-applied CPU-side and we
  // want the bottom-left of the upscaled cell.
  return uint2((u_resolution_scale == 1u) ? round(coords) : floor(coords));
}

// PSX 16-bit RGBA5551 pack from RGBA8. Used by the palette lookup
// path to extract the 5-5-5-1 VRAM value before the per-subpixel
// index extraction.
uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(round(v.r * 31.0));
  uint g = uint(round(v.g * 31.0));
  uint b = uint(round(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return r | (g << 5) | (b << 10) | (a << 15);
}

// VRAM atlas sample. Three structural branches selected at fxc time:
//   PALETTE_4_BIT: 4-bit-paletted texture (16 entries, 4 subpixels)
//   PALETTE_8_BIT: 8-bit-paletted texture (256 entries, 2 subpixels)
//   neither:       direct 16bpp (render-to-texture)
// PALETTE_4_BIT and PALETTE_8_BIT are mutually exclusive (#elif
// chain) so the variant matrix has 3 texture-mode rows, not 4.
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

  // Load palette index from the packed VRAM cell.
  uint2 vicoord = uint2(texpage.x + index_coord.x * u_resolution_scale,
                        texpage.y + index_coord.y * u_resolution_scale);
  float4 texel = samp0.Sample(samp0_ss, float2(vicoord) * rcp_vram_size);
  uint vram_value = RGBA8ToRGBA5551(texel);

  // Extract subpixel palette index.
#  if PALETTE_4_BIT
  uint subpixel = icoord.x & 3u;
  uint palette_index = (vram_value >> (subpixel * 4u)) & 0x0Fu;
#  elif PALETTE_8_BIT
  uint subpixel = icoord.x & 1u;
  uint palette_index = (vram_value >> (subpixel * 8u)) & 0xFFu;
#  endif

  // Sample palette.
  uint2 palette_icoord = uint2(texpage.z + (palette_index * u_resolution_scale),
                               texpage.w);
  return samp0.Sample(samp0_ss, float2(palette_icoord) * rcp_vram_size);
#else
  // Direct 16bpp: render-to-texture effects, upscaled VRAM coords.
  uint2 icoord = ApplyUpscaledTextureWindow(FloatToIntegerCoords(coords));
  uint2 direct_icoord = uint2(texpage.x + icoord.x, texpage.y + icoord.y);
  return samp0.Sample(samp0_ss, float2(direct_icoord) * rcp_vram_size);
#endif
}

// Interpolation qualifier macro for the v_col0 / v_tex0 inputs.
// None / centroid / sample tri-state with INTERP_SAMPLE winning
// over INTERP_CENTROID when both happen to be set.
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

  // Was a compile-time #if INTERLACING guard pre-eb42ffb. Runtime
  // cbuffer branch now; HLSL short-circuits && so the y-LSB
  // arithmetic only runs when interlacing is on.
  if (u_interlacing != 0u && (uint(v_pos.y) & 1u) == u_interlaced_displayed_field)
    discard;

  // Texture coords. Palette textures index the VRAM atlas with
  // native-resolution coords; direct-16bpp textures use upscaled
  // coords. The /= u_resolution_scale path applies to palette modes
  // only.
  float2 coords = v_tex0;
#if PALETTE_4_BIT || PALETTE_8_BIT
  coords /= float(u_resolution_scale);
#endif

  // UV-clamping path. Pre-UV_LIMITS-routing this was gated by the
  // compile-time UV_LIMITS macro. Now u_uv_limits is a runtime
  // cbuffer scalar - 1 when PGXP is on (under any filter) OR the
  // texture filter is non-Nearest (which can't happen in this
  // Nearest template, so for this FS u_uv_limits is effectively
  // "is PGXP on"). When u_uv_limits is 0, v_uv_limits contents
  // (potentially zero, since ComputePolygonUVLimits didn't run on
  // the vertex) are not read.
  float4 texcol;
  if (u_uv_limits != 0u)
  {
    float4 uv_limits = v_uv_limits;
#if !(PALETTE_4_BIT || PALETTE_8_BIT)
    // Direct-16bpp: extend the UV range to all upscaled pixels so
    // 1-pixel-high polygon-based framebuffer effects (e.g. MegaMan
    // Legends 2 haze) are not downsampled.
    uv_limits = uv_limits * float(u_resolution_scale);
    uv_limits.zw += float(u_resolution_scale - 1u);
#endif
    texcol = SampleFromVRAM(v_texpage, clamp(coords, uv_limits.xy, uv_limits.zw));
  }
  else
  {
    texcol = SampleFromVRAM(v_texpage, coords);
  }

  // Transparent-texel discard: VRAM rgba 0000h is the "no draw"
  // sentinel. Standard PSX texture-transparency semantics.
  if (VECTOR_EQ(texcol, float4(0.0, 0.0, 0.0, 0.0)))
    discard;
  float ialpha = 1.0;

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

  // Mask bit: textured polygons mask if either u_set_mask_while_drawing
  // is set or the texel was semitransparent.
  float oalpha = u_set_mask_while_drawing ? 1.0 : (semitransparent ? 1.0 : 0.0);

  // Premultiplied colour. Pre-c532a34 this was a compile-time
  // `#if TRANSPARENCY` branch. Post-routing it's the same runtime
  // test on the u_render_mode cbuffer scalar.
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

  // Output. Textured + transparency arm with the inner OnlyOpaque /
  // OnlyTransparent discards. Pre-c532a34 this was a compile-time
  // `#if TRANSPARENCY ... #else` tri-state with inner
  // `#if TRANSPARENCY_ONLY_OPAQUE / _ONLY_TRANSPARENT` discards.
  // Post-routing it's a single runtime branch on u_render_mode with
  // the discards gated by (u_render_mode == 2u) / (== 3u).
  if (u_render_mode != 0u)
  {
    if (semitransparent)
    {
      o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
#endif
      // OnlyOpaque (u_render_mode == 2): keep opaque pixels only,
      // discard semitransparent ones.
      if (u_render_mode == 2u)
        discard;
    }
    else
    {
      o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
      o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif
      // OnlyTransparent (u_render_mode == 3): keep semitransparent
      // pixels only, discard opaque ones.
      if (u_render_mode == 3u)
        discard;
    }
  }
  else
  {
    // Non-transparency arm. Blending is disabled by the PSO state
    // here so the mask alpha sits directly in the colour write.
    o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
    o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif
  }

  // SV_Depth output - always declared (was conditional on
  // !PGXP_DEPTH pre-routing, see commits 49c0f82 / 116a70e). The
  // end-of-main ternary on u_pgxp_depth picks v_pos.z (PGXP-replayed
  // pass-through) or oalpha * v_pos.z (legacy mask-bit-into-depth
  // encoding). PSO depth comparison func remains PGXP-dependent
  // (LESS_EQUAL vs GREATER_EQUAL set in GPU_HW_D3D12::GetBatchPipeline
  // per m_pgxp_depth_buffer); the FS bytecode is invariant across
  // the flip so a single pre-baked variant serves both PGXP values.
  o_depth = (u_pgxp_depth != 0u) ? v_pos.z : (oalpha * v_pos.z);
}
