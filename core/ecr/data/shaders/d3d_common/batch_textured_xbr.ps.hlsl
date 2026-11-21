// Pre-baked batch fragment shader (textured + xBR filter
// family) for the D3D backends.
//
// Equivalent to GPU_HW_ShaderGen::GenerateBatchFragmentShader
// invoked with texture_mode != GPUTextureMode::Disabled AND
// m_texture_filter in {xBR, xBRBinAlpha}. The two filter enum
// values share a single HLSL source with a BINALPHA -D macro
// gating the alpha quantisation step at the end of
// FilteredSampleFromVRAM. Mirror of the Vulkan template at
// data/shaders/vulkan/batch_textured_xbr.frag.glsl in HLSL.
//
// Fifth and final batch FS template to be pre-baked, after
// batch_untextured (9e6f933 + b6d1903 + c01f8ae),
// batch_textured_nearest (9e4c33d + a7f5717),
// batch_textured_bilinear (269a2a0 + f2ae92a), and
// batch_textured_jinc2 (17a0c66 + 6afe8c2). Once this slice is
// activated, the shadergen + D3DCompile fallback arm in
// GetBatchPipeline / GetBatchPixelShader becomes unreachable and
// is deleted along with the now-redundant tmp_shadergen helper
// on the D3D11 side.
//
// xBR (eXtended Block Resampling) is an edge-detection-based
// scaling algorithm that samples a 5x5 neighbourhood around each
// source texel, computes YCbCr distances between adjacent samples,
// and decides per-quadrant whether to blend at three levels
// (BLEND_NONE / BLEND_NORMAL / BLEND_DOMINANT). The HLSL body is
// substantially larger than Bilinear / JINC2 - it carries the 5x5
// sample fan-out, the per-quadrant gradient comparison, the
// blend-direction calculation, and the line-blend special cases.
// Per-variant DXBC is the largest of any filter template.
//
// Variant axes (5, all `-D` to fxc; post-c532a34 TRANSPARENCY
// routing):
//
//   * Texture mode (6 combos, 3 `-D` macros): same encoding as
//     the textured Nearest / Bilinear / JINC2 templates.
//
//   * USE_DUAL_SOURCE (0/1): drives o_col1 declaration + write.
//     For xBR-family filters, use_dual_source is true whenever
//     m_supports_dual_source_blend is true.
//
//   * INTERP_CENTROID / INTERP_SAMPLE: input interpolation
//     qualifier (none / centroid / sample tri-state).
//
//   * NOPERSP (0/1): `noperspective` qualifier on v_col0.
//
//   * BINALPHA (0/1): gates the ialpha >= 0.5 quantisation step.
//     When 0 (xBR), the blend-weighted alpha passes through as a
//     smooth value; when 1 (xBRBinAlpha), it's quantised to {0, 1}
//     before the body's `ialpha < 0.5 ? discard : ...` test. fxc
//     dead-strips the unused arm at compile time.
//
// 6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) x 2 (BinAlpha)
// = 144 blobs. Same axis cardinality as Bilinear / JINC2; only
// the FilteredSampleFromVRAM body differs. Per-variant DXBC
// expected to land in the 40-60 KiB range, ~2x JINC2's. The MSAA
// axis still does NOT multiply this template.
//
// UV_LIMITS is implicit for all non-Nearest filters (ShouldUseUVLimits
// forces m_using_uv_limits true). The shader unconditionally clamps
// to v_uv_limits without a runtime cbuffer gate.
//
// Variant suffix: pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_b{0,1}.
// The `b` digit encodes BINALPHA (0 = xBR, 1 = xBRBinAlpha).
// Alphabetical sort matches the natural nested-loop iteration
// order in the matching helper.
//
// One documented behavioural quirk is preserved: the per-texel
// "non-transparent" flag for the I sample (lower-right corner of
// the 3x3) is computed from H's vec4 rather than I's. This is a
// shadergen bug in the original PCSX2 xBR implementation that we
// reproduce byte-for-byte so the pre-baked DXBC produces visually
// identical output to the runtime D3DCompile path.

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

// xBR (eXtended Block Resampling) constants and helpers. Body
// lifted from gpu_hw_shadergen.cpp::WriteBatchTextureFilter for
// the xBR / xBRBinAlpha cases. The BINALPHA -D macro merges the
// two enum values into a single template with fxc dead-stripping
// the unused arm.

CONSTANT int   BLEND_NONE                  = 0;
CONSTANT int   BLEND_NORMAL                = 1;
CONSTANT int   BLEND_DOMINANT              = 2;
CONSTANT float LUMINANCE_WEIGHT            = 1.0;
CONSTANT float EQUAL_COLOR_TOLERANCE       = 0.1176470588235294;
CONSTANT float STEEP_DIRECTION_THRESHOLD   = 2.2;
CONSTANT float DOMINANT_DIRECTION_THRESHOLD = 3.6;
CONSTANT float4 W_YCBCR                    = float4(0.2627, 0.6780, 0.0593, 0.5);

float DistYCbCr(float4 pixA, float4 pixB)
{
  const float scaleB = 0.5 / (1.0 - W_YCBCR.b);
  const float scaleR = 0.5 / (1.0 - W_YCBCR.r);
  float4 diff = pixA - pixB;
  float Y  = dot(diff, W_YCBCR);
  float Cb = scaleB * (diff.b - Y);
  float Cr = scaleR * (diff.r - Y);
  return sqrt(((LUMINANCE_WEIGHT * Y) * (LUMINANCE_WEIGHT * Y)) + (Cb * Cb) + (Cr * Cr));
}

bool IsPixEqual(float4 pixA, float4 pixB)
{
  return (DistYCbCr(pixA, pixB) < EQUAL_COLOR_TOLERANCE);
}

float get_left_ratio(float2 center, float2 origin, float2 direction, float2 scale_in)
{
  float2 P0    = center - origin;
  float2 proj  = direction * (dot(P0, direction) / dot(direction, direction));
  float2 distv = P0 - proj;
  float2 orth  = float2(-direction.y, direction.x);
  float side = sign(dot(P0, orth));
  float v    = side * length(distv * scale_in);
  return smoothstep(-sqrt(2.0) / 2.0, sqrt(2.0) / 2.0, v);
}

#define P(coord, xoffs, yoffs) SampleFromVRAM(texpage, clamp(coord + float2(float(xoffs), float(yoffs)), uv_limits.xy, uv_limits.zw))

void FilteredSampleFromVRAM(uint4 texpage, float2 coords, float4 uv_limits,
                            out float4 texcol, out float ialpha)
{
  float2 scale_v = float2(8.0, 8.0);
  float2 pos = frac(coords.xy) - float2(0.5, 0.5);
  float2 coord = coords.xy - pos;

  // Sample 3x3 neighbourhood; W field carries the
  // non-transparent flag (1.0 if any RGB or A component is
  // non-zero, 0.0 otherwise).
  float4 A = P(coord, -1, -1); float Aw = A.w; A.w = VECTOR_NEQ(A, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 B = P(coord,  0, -1); float Bw = B.w; B.w = VECTOR_NEQ(B, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 C = P(coord,  1, -1); float Cw = C.w; C.w = VECTOR_NEQ(C, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 D = P(coord, -1,  0); float Dw = D.w; D.w = VECTOR_NEQ(D, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 E = P(coord,  0,  0); float Ew = E.w; E.w = VECTOR_NEQ(E, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 F = P(coord,  1,  0); float Fw = F.w; F.w = VECTOR_NEQ(F, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 G = P(coord, -1,  1); float Gw = G.w; G.w = VECTOR_NEQ(G, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  float4 H = P(coord,  0,  1); float Hw = H.w; H.w = VECTOR_NEQ(H, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;
  // NOTE: the original shadergen uses VECTOR_NEQ(H, 0) for the
  // Iw flag - this is a documented quirk in the runtime
  // shadergen output that we preserve byte-for-byte so the pre-
  // baked DXBC produces visually identical output to the runtime
  // D3DCompile path.
  float4 I = P(coord,  1,  1); float Iw = I.w; I.w = VECTOR_NEQ(H, float4(0.0, 0.0, 0.0, 0.0)) ? 1.0 : 0.0;

  // blendResult mapping by quadrant index:
  //   x = upper-left
  //   y = upper-right
  //   z = lower-right
  //   w = lower-left
  int4 blendResult = int4(BLEND_NONE, BLEND_NONE, BLEND_NONE, BLEND_NONE);

  // Quadrant z (lower-right)
  if (!((VECTOR_EQ(E, F) && VECTOR_EQ(H, I)) || (VECTOR_EQ(E, H) && VECTOR_EQ(F, I))))
  {
    float dist_H_F = DistYCbCr(G, E) + DistYCbCr(E, C) + DistYCbCr(P(coord, 0, 2), I) + DistYCbCr(I, P(coord, 2, 0)) + (4.0 * DistYCbCr(H, F));
    float dist_E_I = DistYCbCr(D, H) + DistYCbCr(H, P(coord, 1, 2)) + DistYCbCr(B, F) + DistYCbCr(F, P(coord, 2, 1)) + (4.0 * DistYCbCr(E, I));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_H_F) < dist_E_I;
    blendResult.z = ((dist_H_F < dist_E_I) && VECTOR_NEQ(E, F) && VECTOR_NEQ(E, H)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Quadrant w (lower-left)
  if (!((VECTOR_EQ(D, E) && VECTOR_EQ(G, H)) || (VECTOR_EQ(D, G) && VECTOR_EQ(E, H))))
  {
    float dist_G_E = DistYCbCr(P(coord, -2, 1), D) + DistYCbCr(D, B) + DistYCbCr(P(coord, -1, 2), H) + DistYCbCr(H, F) + (4.0 * DistYCbCr(G, E));
    float dist_D_H = DistYCbCr(P(coord, -2, 0), G) + DistYCbCr(G, P(coord, 0, 2)) + DistYCbCr(A, E) + DistYCbCr(E, I) + (4.0 * DistYCbCr(D, H));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_H) < dist_G_E;
    blendResult.w = ((dist_G_E > dist_D_H) && VECTOR_NEQ(E, D) && VECTOR_NEQ(E, H)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Quadrant y (upper-right)
  if (!((VECTOR_EQ(B, C) && VECTOR_EQ(E, F)) || (VECTOR_EQ(B, E) && VECTOR_EQ(C, F))))
  {
    float dist_E_C = DistYCbCr(D, B) + DistYCbCr(B, P(coord, 1, -2)) + DistYCbCr(H, F) + DistYCbCr(F, P(coord, 2, -1)) + (4.0 * DistYCbCr(E, C));
    float dist_B_F = DistYCbCr(A, E) + DistYCbCr(E, I) + DistYCbCr(P(coord, 0, -2), C) + DistYCbCr(C, P(coord, 2, 0)) + (4.0 * DistYCbCr(B, F));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_B_F) < dist_E_C;
    blendResult.y = ((dist_E_C > dist_B_F) && VECTOR_NEQ(E, B) && VECTOR_NEQ(E, F)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  // Quadrant x (upper-left)
  if (!((VECTOR_EQ(A, B) && VECTOR_EQ(D, E)) || (VECTOR_EQ(A, D) && VECTOR_EQ(B, E))))
  {
    float dist_D_B = DistYCbCr(P(coord, -2, 0), A) + DistYCbCr(A, P(coord, 0, -2)) + DistYCbCr(G, E) + DistYCbCr(E, C) + (4.0 * DistYCbCr(D, B));
    float dist_A_E = DistYCbCr(P(coord, -2, -1), D) + DistYCbCr(D, H) + DistYCbCr(P(coord, -1, -2), B) + DistYCbCr(B, F) + (4.0 * DistYCbCr(A, E));
    bool dominantGradient = (DOMINANT_DIRECTION_THRESHOLD * dist_D_B) < dist_A_E;
    blendResult.x = ((dist_D_B < dist_A_E) && VECTOR_NEQ(E, D) && VECTOR_NEQ(E, B)) ? (dominantGradient ? BLEND_DOMINANT : BLEND_NORMAL) : BLEND_NONE;
  }

  float4 res = E;
  float resW = Ew;

  // Quadrant z (lower-right) blend
  if (blendResult.z != BLEND_NONE)
  {
    float dist_F_G = DistYCbCr(F, G);
    float dist_H_C = DistYCbCr(H, C);
    bool doLineBlend = (blendResult.z == BLEND_DOMINANT ||
      !((blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) || (blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) ||
        (IsPixEqual(G, H) && IsPixEqual(H, I) && IsPixEqual(I, F) && IsPixEqual(F, C) && !IsPixEqual(E, I))));

    float2 origin = float2(0.0, 1.0 / sqrt(2.0));
    float2 direction = float2(1.0, -1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_F_G <= dist_H_C) && VECTOR_NEQ(E, G) && VECTOR_NEQ(D, G);
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_H_C <= dist_F_G) && VECTOR_NEQ(E, C) && VECTOR_NEQ(B, C);
      origin = haveShallowLine ? float2(0.0, 0.25) : float2(0.0, 0.5);
      direction.x += haveShallowLine ? 1.0 : 0.0;
      direction.y -= haveSteepLine   ? 1.0 : 0.0;
    }

    float4 blendPix = lerp(H, F, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    float blendW    = lerp(Hw, Fw, step(DistYCbCr(E, F), DistYCbCr(E, H)));
    res  = lerp(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = lerp(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Quadrant w (lower-left) blend
  if (blendResult.w != BLEND_NONE)
  {
    float dist_H_A = DistYCbCr(H, A);
    float dist_D_I = DistYCbCr(D, I);
    bool doLineBlend = (blendResult.w == BLEND_DOMINANT ||
      !((blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) || (blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) ||
        (IsPixEqual(A, D) && IsPixEqual(D, G) && IsPixEqual(G, H) && IsPixEqual(H, I) && !IsPixEqual(E, G))));

    float2 origin = float2(-1.0 / sqrt(2.0), 0.0);
    float2 direction = float2(1.0, 1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_H_A <= dist_D_I) && VECTOR_NEQ(E, A) && VECTOR_NEQ(B, A);
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_D_I <= dist_H_A) && VECTOR_NEQ(E, I) && VECTOR_NEQ(F, I);
      origin = haveShallowLine ? float2(-0.25, 0.0) : float2(-0.5, 0.0);
      direction.y += haveShallowLine ? 1.0 : 0.0;
      direction.x += haveSteepLine   ? 1.0 : 0.0;
    }

    float4 blendPix = lerp(H, D, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    float blendW    = lerp(Hw, Dw, step(DistYCbCr(E, D), DistYCbCr(E, H)));
    res  = lerp(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = lerp(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Quadrant y (upper-right) blend
  if (blendResult.y != BLEND_NONE)
  {
    float dist_B_I = DistYCbCr(B, I);
    float dist_F_A = DistYCbCr(F, A);
    bool doLineBlend = (blendResult.y == BLEND_DOMINANT ||
      !((blendResult.x != BLEND_NONE && !IsPixEqual(E, I)) || (blendResult.z != BLEND_NONE && !IsPixEqual(E, A)) ||
        (IsPixEqual(I, F) && IsPixEqual(F, C) && IsPixEqual(C, B) && IsPixEqual(B, A) && !IsPixEqual(E, C))));

    float2 origin = float2(1.0 / sqrt(2.0), 0.0);
    float2 direction = float2(-1.0, -1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_B_I <= dist_F_A) && VECTOR_NEQ(E, I) && VECTOR_NEQ(H, I);
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_F_A <= dist_B_I) && VECTOR_NEQ(E, A) && VECTOR_NEQ(D, A);
      origin = haveShallowLine ? float2(0.25, 0.0) : float2(0.5, 0.0);
      direction.y -= haveShallowLine ? 1.0 : 0.0;
      direction.x -= haveSteepLine   ? 1.0 : 0.0;
    }

    float4 blendPix = lerp(F, B, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    float blendW    = lerp(Fw, Bw, step(DistYCbCr(E, B), DistYCbCr(E, F)));
    res  = lerp(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = lerp(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Quadrant x (upper-left) blend
  if (blendResult.x != BLEND_NONE)
  {
    float dist_D_C = DistYCbCr(D, C);
    float dist_B_G = DistYCbCr(B, G);
    bool doLineBlend = (blendResult.x == BLEND_DOMINANT ||
      !((blendResult.w != BLEND_NONE && !IsPixEqual(E, C)) || (blendResult.y != BLEND_NONE && !IsPixEqual(E, G)) ||
        (IsPixEqual(C, B) && IsPixEqual(B, A) && IsPixEqual(A, D) && IsPixEqual(D, G) && !IsPixEqual(E, A))));

    float2 origin = float2(0.0, -1.0 / sqrt(2.0));
    float2 direction = float2(-1.0, 1.0);
    if (doLineBlend)
    {
      bool haveShallowLine = (STEEP_DIRECTION_THRESHOLD * dist_D_C <= dist_B_G) && VECTOR_NEQ(E, C) && VECTOR_NEQ(F, C);
      bool haveSteepLine   = (STEEP_DIRECTION_THRESHOLD * dist_B_G <= dist_D_C) && VECTOR_NEQ(E, G) && VECTOR_NEQ(H, G);
      origin = haveShallowLine ? float2(0.0, -0.25) : float2(0.0, -0.5);
      direction.x -= haveShallowLine ? 1.0 : 0.0;
      direction.y += haveSteepLine   ? 1.0 : 0.0;
    }

    float4 blendPix = lerp(D, B, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    float blendW    = lerp(Dw, Bw, step(DistYCbCr(E, B), DistYCbCr(E, D)));
    res  = lerp(res,  blendPix, get_left_ratio(pos, origin, direction, scale_v));
    resW = lerp(resW, blendW,   get_left_ratio(pos, origin, direction, scale_v));
  }

  // Note: Iw is computed (for parity with the Vulkan template's
  // structure) but never referenced in any blend formula - fxc
  // dead-strips the assignment.

  ialpha = res.w;
  texcol = float4(res.xyz, resW);

  if (ialpha > 0.0)
    texcol.rgb /= float3(ialpha, ialpha, ialpha);

  // BinAlpha: quantise the blend-weighted alpha to {0, 1}.
  // Drops the smooth-alpha behaviour at edge transitions in
  // favour of crisp alpha cutouts.
#if BINALPHA
  ialpha = (ialpha >= 0.5) ? 1.0 : 0.0;
#endif
}

#undef P

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
