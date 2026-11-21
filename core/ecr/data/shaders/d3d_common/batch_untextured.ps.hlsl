// Pre-baked batch fragment shader (untextured slice) for the D3D12
// backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateBatchFragmentShader invoked
// with texture_mode == GPUTextureMode::Disabled. This is the smallest
// of the 5 batch FS templates - the textured path emits SampleFromVRAM
// and a much larger ApplyTextureFilter section that lives in the four
// filter-family templates (Nearest / Bilinear / JINC2 / xBR).
//
// First batch-FS pre-bake exercise. The Vulkan side already
// pre-bakes 60 batch FS SPIR-V blobs across 5 templates (post-arc:
// dae19ff); the D3D12 side has been runtime-D3DCompile-only up to this
// point. The end-state goal is to eliminate D3DCompile from the
// runtime entirely for batch shaders, matching how the non-batch VRAM
// ops shaders (vram_copy / vram_fill / vram_write / vram_read /
// vram_update_depth / display) already work.
//
// Post-TRANSPARENCY-routing variant matrix. The 4-state
// BatchRenderMode enum (TransparencyDisabled / TransparentAndOpaque /
// OnlyOpaque / OnlyTransparent) used to drive a compile-time
// TRANSPARENCY `-D` axis on this template, but landed at commit
// c532a34 ("core: route TRANSPARENCY through the batch UBO") as a
// cbuffer scalar `u_render_mode` shared across all backends. The
// runtime branch (u_render_mode != 0u) replaces the former
// `#if TRANSPARENCY` body guards. The inner OnlyOpaque /
// OnlyTransparent discards (former TRANSPARENCY_ONLY_OPAQUE /
// TRANSPARENCY_ONLY_TRANSPARENT macros) are unreachable for
// untextured per the shadergen long-standing rule "We shouldn't be
// rendering opaque geometry only when untextured" (see
// gpu_hw_shadergen.cpp). So this template emits a single colour
// path with the only render-mode-dependent computation being the
// premultiply factor.
//
// Variant axes (4, all `-D` to fxc):
//
//   * USE_DUAL_SOURCE: 0 or 1. Drives the second colour output
//     declaration (o_col1 at location 0 index 1) and the matching
//     SRC1_COLOR / SRC1_ALPHA write. Per-call value derived in C++
//     from m_supports_dual_source_blend AND ((transparency !=
//     TransparencyDisabled && transparency != OnlyOpaque) ||
//     m_texture_filter != Nearest). For untextured the filter check
//     still gates (a filter setting of Bilinear / JINC2 / xBR forces
//     dual_source=true even when transparency=Disabled), so both
//     values are reachable.
//
//   * INTERP_CENTROID / INTERP_SAMPLE: input interpolation qualifier.
//     Mutually exclusive booleans encoding the (none / centroid /
//     sample) tri-state. INTERP_SAMPLE wins if both are set. Maps to
//     ShaderGen::GetInterpolationQualifier with msaa /
//     per_sample_shading derived from m_multisamples > 1 and
//     m_per_sample_shading respectively.
//
//   * NOPERSP: 0 or 1. Adds `noperspective` to the v_col0 input
//     qualifier. Set when m_disable_color_perspective is true (PGXP
//     texture correction enabled without colour correction - niche).
//
// Total: 2 (dual) x 3 (interp) x 2 (persp) = 12 blobs. Down from
// 24 pre-routing - the transparency axis collapsed onto the
// single runtime branch on u_render_mode below.
//
// The MSAA axis (m_multisamples 1/2/4/8/16/32) does NOT multiply this
// template because the untextured FS body has no LOAD_TEXTURE_MS
// sample-resolve loop to unroll - MSAA only affects the input
// qualifier choice (centroid / sample), which the INTERP_* axes
// already capture. The textured filter templates DO multiply by
// MULTISAMPLES for their LOAD_TEXTURE_MS unroll counts.

#define HLSL 1
#define CONSTANT static const
#define VECTOR_EQ(a, b) (all((a) == (b)))

cbuffer UBOBlock : register(b0)
{
  // 64-byte cbuffer. C++ side is GPU_HW::BatchUBOData (gpu_hw.h:111);
  // field order MUST match the struct member order there.
  //
  // The trailing u_uv_limits is read by the textured-Nearest FS only
  // (and only when u_uv_limits != 0u); on the untextured path it's
  // present in the cbuffer but unused. u_render_mode (former u_pad2
  // pre-c532a34) is read at the premultiply site below as a runtime
  // branch on the 4-state BatchRenderMode enum.
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
  // u_scaled_dithering chooses between native-PSX-grid dithering
  // (matching the 1x-resolution rate) and screen-space dithering
  // (matching the upscaled rate). u_resolution_scale==1u makes the
  // two paths identical; at higher scales they diverge - screen-
  // space is "more dither", native is the closer-to-stock-look
  // path.
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

// Interpolation qualifier macro for the v_col0 input. None / centroid /
// sample tri-state with INTERP_SAMPLE winning over INTERP_CENTROID
// when both happen to be set.
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
  COLOR_INTERP in float4 v_col0 : COLOR0,
  in float4 v_pos : SV_Position,
  out float o_depth : SV_Depth,
  out float4 o_col0 : SV_Target0
#if USE_DUAL_SOURCE
  , out float4 o_col1 : SV_Target1
#endif
  )
{
  uint3 vertcol = uint3(v_col0.rgb * float3(255.0, 255.0, 255.0));

  bool semitransparent;
  uint3 icolor;
  float ialpha;
  float oalpha;

  // Was a compile-time #if INTERLACING guard. Now u_interlacing is a
  // cbuffer scalar (0 = off, 1 = on) and the inner LSB compare gates
  // on it. HLSL short-circuits &&, so the y-LSB arithmetic only runs
  // when interlacing is actually on. Mirrors the C++ shadergen
  // routing.
  if (u_interlacing != 0u && (uint(v_pos.y) & 1u) == u_interlaced_displayed_field)
    discard;

  // Untextured: all pixels are semitransparent, ialpha = 1, icolor =
  // vertcol, with dithering / true_color cbuffer branches deciding
  // the final colour quantisation. Mirrors the #else branch of the
  // TEXTURED guard in the C++ shadergen.
  semitransparent = true;
  icolor = vertcol;
  ialpha = 1.0;

  if (u_dithering != 0u)
  {
    icolor = ApplyDithering(uint2(v_pos.xy), icolor);
  }
  else
  {
    if (u_true_color == 0u)
      icolor >>= 3;
  }

  // Mask bit output. u_set_mask_while_drawing == true forces 1.0
  // (set-mask-on-draw mode); otherwise this writes the
  // semitransparent flag (which is always 1 for untextured, so this
  // is effectively `float(u_set_mask_while_drawing) || 1.0` - the
  // shadergen original keeps the cast structure for cross-backend
  // bit-for-bit reproducibility).
  oalpha = float(u_set_mask_while_drawing);

  // Premultiply alpha so we don't need a separate colour-output
  // channel for it. Pre-c532a34 this was a compile-time
  // `#if TRANSPARENCY` branch (the macro encoded `render_mode !=
  // TransparencyDisabled`). Post-routing it's the same runtime test
  // on the u_render_mode cbuffer scalar - DXBC compiles to a single
  // uniform-flow branch since every lane in a draw shares the same
  // u_render_mode value.
  float premultiply_alpha = ialpha;
  if (u_render_mode != 0u)
    premultiply_alpha = ialpha * (semitransparent ? u_src_alpha_factor : 1.0);

  float3 color;
  if (u_true_color != 0u)
  {
    // True colour: preserve 8-bit-per-channel precision into the
    // premultiplied output. The /255 keeps the SDR colour space.
    color = (float3(icolor) * premultiply_alpha) / float3(255.0, 255.0, 255.0);
  }
  else
  {
    // PSX-native 5-bit-per-channel framebuffer. Premultiply BEFORE
    // truncating to /31 so the blend unit doesn't carry 32-bit
    // precision through a 16-bit blend operation.
    color = floor(float3(icolor) * premultiply_alpha) / float3(31.0, 31.0, 31.0);
  }

  // Untextured output. The TRANSPARENCY && TEXTURED arm of the
  // shadergen original is unreachable here (we're the !TEXTURED
  // slice); the elif TRANSPARENCY arm and the else arm collapse onto
  // a single colour-write site with the o_col1 expression chosen by
  // the runtime u_render_mode branch when dual_source is on.
  // Untextured opaque-only is a documented no-op per the shadergen
  // comment ("We shouldn't be rendering opaque geometry only when
  // untextured"), so we don't gate the colour write on
  // (u_render_mode == 2u) - it'd be unreachable noise.
  o_col0 = float4(color, oalpha);
#if USE_DUAL_SOURCE
  if (u_render_mode != 0u)
    o_col1 = float4(0.0, 0.0, 0.0, u_dst_alpha_factor / ialpha);
  else
    o_col1 = float4(0.0, 0.0, 0.0, 1.0 - ialpha);
#endif

  // SV_Depth output - always declared (was conditional on !PGXP_DEPTH
  // pre-routing, see commits 49c0f82 / 116a70e). The end-of-main
  // ternary on u_pgxp_depth picks v_pos.z (PGXP-replayed pass-
  // through) or oalpha * v_pos.z (legacy mask-bit-into-depth
  // encoding). PSO depth comparison func remains PGXP-dependent
  // (LESS_EQUAL vs GREATER_EQUAL set in GPU_HW_D3D12::GetBatchPipeline
  // per m_pgxp_depth_buffer); the FS bytecode is invariant across
  // the flip so the existing m_batch_fragment_shader_blobs cache
  // serves both PGXP values.
  o_depth = (u_pgxp_depth != 0u) ? v_pos.z : (oalpha * v_pos.z);
}
