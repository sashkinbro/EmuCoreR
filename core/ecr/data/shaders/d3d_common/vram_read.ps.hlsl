// Pre-baked VRAM read pixel shader for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader()
// in src/core/gpu_hw_shadergen.cpp post-2980961 cbuffer routing.
// Used by GPU_HW_D3D12::GetVRAMReadbackPipeline for the upscaled-
// VRAM-to-native-16bpp encode pass (screenshot capture, libretro
// readback API, save-state staging). Not in the per-frame hot
// path - this only runs when the frontend explicitly requests
// VRAM contents - but every variant of it must be available
// because m_multisamples and per-game MSAA settings determine
// which one's needed.
//
// Variant axis: MULTISAMPLES = 1, 2, 4, 8, 16, or 32. Six .inc
// blobs:
//   vram_read_ps_m1.inc   (MULTISAMPLES = 1,  no MSAA)
//   vram_read_ps_m2.inc   (MULTISAMPLES = 2,  2x MSAA)
//   vram_read_ps_m4.inc   (MULTISAMPLES = 4,  4x MSAA)
//   vram_read_ps_m8.inc   (MULTISAMPLES = 8,  8x MSAA)
//   vram_read_ps_m16.inc  (MULTISAMPLES = 16, 16x MSAA)
//   vram_read_ps_m32.inc  (MULTISAMPLES = 32, 32x MSAA)
//
// First MSAA-count cardinality variant. Where
// vram_update_depth_ps (91f8c32) split on a binary
// MULTISAMPLING flag (on/off), vram_read_ps splits on the
// MULTISAMPLES *count* itself - the [unroll] loop in LoadVRAM
// unrolls a different number of times per variant, so each
// power-of-2 sample count produces a distinct DXBC. The m1
// blob takes the #else path (Texture2D, single Load); the
// m2..m32 blobs all use Texture2DMS<float4> but with
// different unroll counts. PSO MSAA configuration
// (gpbuilder.SetMultisamples) must match the shader's
// MULTISAMPLES constant or per-sample loads will read from
// the wrong sample slices.
//
// MULTISAMPLES is supplied as a /D MULTISAMPLES=N define on
// the fxc command line. MULTISAMPLING derives from
// MULTISAMPLES > 1 in the #if block below - same predicate
// as the shadergen UsingMSAA() helper.

#if MULTISAMPLES > 1
  #define MULTISAMPLING 1
#else
  #define MULTISAMPLING 0
#endif

cbuffer UBOBlock : register(b0)
{
  // 24-byte cbuffer. vram_read_ps has no formal UBOData struct
  // in gpu_hw.h - the call site at GPU_HW_D3D12::ReadVRAM
  // pushes 6 uint DWORDs (u_base_coords.xy, u_size.xy,
  // u_resolution_scale, u_pad0) via SetGraphicsRoot32BitConstants.
  // The field order here MUST match those push offsets.
  uint2 u_base_coords;
  uint2 u_size;
  uint u_resolution_scale;
  uint u_pad0;
};

// RESOLUTION_SCALE alias - mirrors the
// WriteCBufferResolutionScaleAliases helper's #define output in
// gpu_hw_shadergen.cpp. The body's `if (RESOLUTION_SCALE == 1u)`
// fast-path and the `for (offset < RESOLUTION_SCALE; ...)` box-
// filter loops are now runtime branches on u_resolution_scale.
// The loops can no longer be fully unrolled at compile time, but
// vram_read_ps is on-demand (screenshot / readback) not per-frame,
// so the runtime loop overhead is irrelevant compared to the
// texture-load cost.
#define RESOLUTION_SCALE u_resolution_scale

#if MULTISAMPLING
  Texture2DMS<float4> samp0 : register(t0);
  SamplerState samp0_ss : register(s0);
#else
  Texture2D samp0 : register(t0);
  SamplerState samp0_ss : register(s0);
#endif

// PSX 16-bit RGBA5551 pack from float4 RGBA8. Same math as the
// RGBA8ToRGBA5551 helper in WriteCommonFunctions; round (HLSL
// banker's rounding) replaces GLSL roundEven (the shadergen
// HLSL prelude #defines roundEven = round, same outcome).
uint RGBA8ToRGBA5551(float4 v)
{
  uint r = uint(round(v.r * 31.0));
  uint g = uint(round(v.g * 31.0));
  uint b = uint(round(v.b * 31.0));
  uint a = (v.a != 0.0) ? 1u : 0u;
  return (r) | (g << 5) | (b << 10) | (a << 15);
}

// Load one upscaled-VRAM sample. On MSAA paths this resolves all
// samples by averaging - the readback texture is single-sample
// so the per-sample data has to collapse here.
float4 LoadVRAM(int2 coords)
{
#if MULTISAMPLING
  // MULTISAMPLES is a compile-time constant from /D, so the loop
  // is fully unrolled. m32 unrolls 32 iterations, m2 unrolls 2.
  // Each iteration is one Texture2DMS<float4>.Load(coords, sample)
  // followed by an accumulate, so the DXBC growth is linear in
  // MULTISAMPLES.
  float4 value = samp0.Load(coords, 0u);
  [unroll] for (uint sample_index = 1u; sample_index < MULTISAMPLES; sample_index++)
    value += samp0.Load(coords, sample_index);
  value /= float(MULTISAMPLES);
  return value;
#else
  // Non-MS path: single mip-0 point fetch. samp0.Load takes
  // (coords.x, coords.y, mip) packed as int3 for Texture2D.
  return samp0.Load(int3(coords, 0));
#endif
}

// Sample upscaled VRAM with downsampling. Fast path for 1x scale;
// box filter for everything else.
uint SampleVRAM(uint2 coords)
{
  if (RESOLUTION_SCALE == 1u)
    return RGBA8ToRGBA5551(LoadVRAM(int2(coords)));

  // Box filter for downsampling. The double loop runs
  // RESOLUTION_SCALE^2 times - 4 iterations at 2x, 16 at 4x,
  // 64 at 8x. These are NOT [unroll]'d because RESOLUTION_SCALE
  // is a cbuffer-routed runtime value post-2980961; HLSL emits
  // a runtime loop. The texture-load cost dwarfs the loop
  // overhead either way.
  float4 value = float4(0.0, 0.0, 0.0, 0.0);
  uint2 base_coords = coords * uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  for (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    for (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)
      value += LoadVRAM(int2(base_coords + uint2(offset_x, offset_y)));
  }
  value /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  return RGBA8ToRGBA5551(value);
}

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
  out float4 o_col0 : SV_Target0)
{
  // Encoding output as 32-bit (RGBA8); the destination texture
  // is half-width because we pack two PSX 16-bit pixels per
  // 32-bit output pixel (left in xy, right in zw). The viewport
  // half-width drives this - see encoded_width math at the
  // GPU_HW_D3D12::ReadVRAM call site.
  uint2 sample_coords = uint2(uint(v_pos.x) * 2u, uint(v_pos.y));
  // OpenGL y-flip omitted - D3D12 uses top-left origin like PSX
  // does, no flip needed (the OpenGL backend's shadergen body
  // does `sample_coords.y = u_size.y - sample_coords.y - 1u`
  // here).
  sample_coords += u_base_coords;

  uint left = SampleVRAM(sample_coords);
  uint right = SampleVRAM(uint2(sample_coords.x + 1u, sample_coords.y));

  // Pack [left_lo, left_hi, right_lo, right_hi] as four 8-bit
  // channels and divide by 255 to normalise to UNORM range.
  o_col0 = float4(float(left & 0xFFu), float((left >> 8) & 0xFFu),
                  float(right & 0xFFu), float((right >> 8) & 0xFFu))
            / float4(255.0, 255.0, 255.0, 255.0);
}
