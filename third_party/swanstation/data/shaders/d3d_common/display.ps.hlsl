// Pre-baked display pixel shader for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateDisplayFragmentShader(
//   depth_24bit, interlace_mode, smooth_chroma)
// in src/core/gpu_hw_shadergen.cpp post-e64fc28 cbuffer routing.
// Used by GPU_HW_D3D12::GetDisplayPipeline for the every-frame
// scanout pass. This is the per-frame hot shader - every frame
// goes through this on its way to the host display, so cold-flip
// cost matters here more than anywhere else in the GPU pipeline.
//
// Variant axes: 4 axes giving 54 unique variants.
//
//   * DEPTH_24BIT (2): 24-bit colour mode (FMV / true-colour
//     framebuffers) vs the common 16-bit framebuffer.
//   * INTERLACED + INTERLEAVED (3 modes packed in 2 bools):
//     - i0 (None):              INTERLACED=0, INTERLEAVED=0
//     - i1 (InterleavedFields): INTERLACED=1, INTERLEAVED=1
//     - i2 (SeparateFields):    INTERLACED=1, INTERLEAVED=0
//   * SMOOTH_CHROMA (2): chroma-plane smoothing in the 24-bit
//     path. Dead code when DEPTH_24BIT=0; the variant table omits
//     the c=1 entry for d=0 (18 + 36 = 54, not 2 x 3 x 2 x 6 = 72).
//   * MULTISAMPLES (6: 1, 2, 4, 8, 16, 32): MSAA sample count.
//     Same cardinality axis as vram_read_ps (21f4623); the
//     [unroll] sample-resolve loop in LoadVRAM unrolls a
//     different number of times per blob.
//
// Variant suffix convention: d{0,1}i{0,1,2}c{0,1}m{01,02,04,08,16,32}.
// MULTISAMPLES is padded to 2 digits so alphabetical .inc filename
// ordering matches the natural nested-loop iteration order. The
// d=0 path uses c=0 only.
//
// Runtime selection in GetDisplayPipeline:
//   if (depth_24) -> k_display_d1[i][c][ms_idx]
//   else          -> k_display_d0[i][ms_idx]
// Two arrays rather than one ragged 4D array, because the c
// dimension is genuinely meaningless when depth_24=0.

#if MULTISAMPLES > 1
  #define MULTISAMPLING 1
#else
  #define MULTISAMPLING 0
#endif

cbuffer UBOBlock : register(b0)
{
  // 24-byte cbuffer. display_ps has no formal UBOData struct in
  // gpu_hw.h - GPU_HW_D3D12::UpdateDisplay pushes 6 uint DWORDs
  // via SetGraphicsRoot32BitConstants. The field order here MUST
  // match those push offsets.
  uint2 u_vram_offset;
  uint u_crop_left;
  uint u_field_offset;
  uint u_resolution_scale;
  uint u_pad0;
};

// RESOLUTION_SCALE / VRAM_SIZE aliases - mirror
// WriteCBufferResolutionScaleAliases's #define output. The body
// uses RESOLUTION_SCALE in SampleVRAM24 coord scaling and
// VRAM_SIZE in the 16-bit path's modulo wrap; both expand to
// arithmetic on the cbuffer-routed u_resolution_scale.
#define RESOLUTION_SCALE u_resolution_scale
#define VRAM_SIZE        (uint2(1024u, 512u) * u_resolution_scale)

#if MULTISAMPLING
  Texture2DMS<float4> samp0 : register(t0);
  SamplerState samp0_ss : register(s0);
#else
  Texture2D samp0 : register(t0);
  SamplerState samp0_ss : register(s0);
#endif

// PSX 16-bit RGBA5551 pack from RGBA8. Inlined from
// WriteCommonFunctions's RGBA8ToRGBA5551 helper.
uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(round(v.r * 31.0));
  uint g = uint(round(v.g * 31.0));
  uint b = uint(round(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

// fixYCoord on D3D12 is identity (top-left origin matches PSX).
// Only the OpenGL backend's GLSL flips here; kept as a 1-line
// helper to make the body match the shadergen path byte-for-byte
// where it's called.
uint fixYCoord(uint y) { return y; }

// BT.601 YUV / RGB matrices - used by the chroma-smoothing path
// to split luma from chroma, smooth the chroma plane only, then
// recombine. Same coefficients as the shadergen body.
float3 RGBToYUV(float3 rgb)
{
  return float3(dot(rgb.rgb, float3(0.299f, 0.587f, 0.114f)),
                dot(rgb.rgb, float3(-0.14713f, -0.28886f, 0.436f)),
                dot(rgb.rgb, float3(0.615f, -0.51499f, -0.10001f)));
}

float3 YUVToRGB(float3 yuv)
{
  return float3(dot(yuv, float3(1.0f, 0.0f, 1.13983f)),
                dot(yuv, float3(1.0f, -0.39465f, -0.58060f)),
                dot(yuv, float3(1.0f, 2.03211f, 0.0f)));
}

// Load one upscaled-VRAM sample, averaging across MSAA samples
// when MULTISAMPLING is on. Same shape as vram_read_ps's LoadVRAM
// - MULTISAMPLES is a compile-time constant from /D, so the loop
// is fully [unroll]'d.
float4 LoadVRAM(int2 coords)
{
#if MULTISAMPLING
  float4 value = samp0.Load(coords, 0u);
  [unroll] for (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += samp0.Load(coords, sample_index);
  value /= float(MULTISAMPLES);
  return value;
#else
  return samp0.Load(int3(coords, 0));
#endif
}

// 24-bit colour sample. PSX 24-bit framebuffers pack 3 bytes per
// pixel across the 16-bit VRAM word stream, so each output pixel
// reads from two adjacent VRAM texels and selects bytes based on
// even/odd input X.
float3 SampleVRAM24(uint2 icoords)
{
  uint2 clamp_size = uint2(1024, 512);
  uint2 vram_coords = u_vram_offset + uint2((icoords.x * 3u) / 2u, icoords.y);
  uint s0 = RGBA8ToRGBA5551(LoadVRAM(int2((vram_coords % clamp_size) * RESOLUTION_SCALE)));
  uint s1 = RGBA8ToRGBA5551(LoadVRAM(int2(((vram_coords + uint2(1, 0)) % clamp_size) * RESOLUTION_SCALE)));
  uint s1s0 = ((s1 << 16) | s0) >> ((icoords.x & 1u) * 8u);
  return float3(float(s1s0 & 0xFFu) / 255.0,
                float((s1s0 >> 8u) & 0xFFu) / 255.0,
                float((s1s0 >> 16u) & 0xFFu) / 255.0);
}

// 2x2 box average over the 24-bit path. Used as the chroma-plane
// kernel by SampleVRAM24Smoothed below.
float3 SampleVRAMAverage2x2(uint2 icoords)
{
  float3 value = SampleVRAM24(icoords);
  value += SampleVRAM24(icoords + uint2(0, 1));
  value += SampleVRAM24(icoords + uint2(1, 0));
  value += SampleVRAM24(icoords + uint2(1, 1));
  return value * 0.25;
}

// Chroma-smoothing: keep luma from the centre tap, replace chroma
// with a bilinear blend of the four surrounding 2x2 averages.
// Heavy - 9 SampleVRAM24 calls per output pixel (1 centre + 4 x
// 2x2 averages), each of which is 2 LoadVRAM calls, each of which
// is MULTISAMPLES texture loads when MSAA is on. At /DSMOOTH_CHROMA=1
// /DMULTISAMPLES=32 that's 9 * 2 * 32 = 576 texture loads per pixel.
// The blob size scales accordingly.
float3 SampleVRAM24Smoothed(uint2 icoords)
{
  int2 base = int2(icoords) - 1;
  uint2 low = uint2(max(base & ~1, int2(0, 0)));
  uint2 high = low + 2u;
  float2 coeff = float2(base & 1) * 0.5 + 0.25;

  float3 p = SampleVRAM24(icoords);
  float3 p00 = SampleVRAMAverage2x2(low);
  float3 p01 = SampleVRAMAverage2x2(uint2(low.x, high.y));
  float3 p10 = SampleVRAMAverage2x2(uint2(high.x, low.y));
  float3 p11 = SampleVRAMAverage2x2(high);

  float3 s = lerp(lerp(p00, p10, coeff.x),
                  lerp(p01, p11, coeff.x),
                  coeff.y);

  float y = RGBToYUV(p).x;
  float2 uv = RGBToYUV(s).yz;
  return YUVToRGB(float3(y, uv));
}

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
  out float4 o_col0 : SV_Target0)
{
  uint2 icoords = uint2(v_pos.xy) + uint2(u_crop_left, 0u);

#if INTERLACED
  // Field-skip: discard the field not currently being scanned out.
  if ((fixYCoord(icoords.y) & 1u) != u_field_offset)
    discard;

  #if !INTERLEAVED
    // SeparateFields: halve Y so the half-height interlaced
    // framebuffer maps to the full output height.
    icoords.y /= 2u;
  #else
    // InterleavedFields: snap Y to even rows so both fields land
    // on the same scanline pair in VRAM.
    icoords.y &= ~1u;
  #endif
#endif

#if DEPTH_24BIT
  #if SMOOTH_CHROMA
    o_col0 = float4(SampleVRAM24Smoothed(icoords), 1.0);
  #else
    o_col0 = float4(SampleVRAM24(icoords), 1.0);
  #endif
#else
  // 16-bit path: direct VRAM read with modulo wrap.
  o_col0 = float4(LoadVRAM(int2((icoords + u_vram_offset) % VRAM_SIZE)).rgb, 1.0);
#endif
}
