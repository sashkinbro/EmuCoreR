// Pre-baked HLSL for
// GPU_HW_ShaderGen::GenerateBoxSampleDownsampleFragmentShader().
//
// Backport of data/shaders/vulkan/box_sample_downsample.frag.glsl
// to HLSL for the D3D backends. Used when m_downsample_mode = Box:
// averages a RESOLUTION_SCALE x RESOLUTION_SCALE block of upscaled
// texels back down to a single PSX-native-resolution pixel.
//
// Variant axis (1 dimension, 15 blobs):
//   RESOLUTION_SCALE (-D macro, integer literal). Drives:
//     - base_coords block-origin scaling
//     - the two inner loop bounds (so fxc can unroll at compile
//       time)
//     - the final averaging divisor (RESOLUTION_SCALE^2)
//
// The Vulkan template uses spec constant id=0 here; on HLSL we
// use -D RESOLUTION_SCALE=N at fxc time so the inner loops become
// fixed-trip-count and fxc fully unrolls them in the DXBC.
// Reachable values for the Box downsample mode are [2,
// m_max_resolution_scale]. With D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
// = 16384 and VRAM_WIDTH = 1024 the cap is 16, so the reachable
// set is {2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
// -> 15 variants. RESOLUTION_SCALE=1 disables downsampling entirely
// (see GPU_HW::GetDownsampleMode in gpu_hw.cpp:399) so we don't
// bake a variant for it.
//
// Note on loop unrolling: at scale=16 the inner body runs 256
// times. fxc unrolls fully when RESOLUTION_SCALE is a literal at
// /D-time, which is fine for this pass - the downsample runs once
// per frame, not per drawcall, and the post-c532a34 throughput
// budget at PSX framerates handles 256 LOAD_TEXTURE ops per
// downsampled pixel comfortably even at 4K + 16x.
//
// Suffix: s{2..16}.
//
// Bindings:
//   Texture2D samp0 at register(t0)   : high-res render target
//   SamplerState samp0_ss at register(s0)
//
// The Box mode does not bind the SmoothingUBOData cbuffer (it's
// only used by the Adaptive mip / blur / composite passes), so
// no cbuffer is declared here.
//
// Box composite reads v_pos.xy (SV_Position) and ignores the
// interpolated texcoord. The unused v_tex0 : TEXCOORD0 input must
// still be declared FIRST so fxc packs SV_Position at input
// register 1, matching the fullscreen-quad vertex shader's output
// signature (TEXCOORD0 -> reg0, SV_Position -> reg1). D3D12's
// CreateGraphicsPipelineState links VS-output to PS-input by
// register; if SV_Position landed at PS input register 0 (which
// happens when TEXCOORD0 is omitted) it would have no matching VS
// output and PSO creation would fail (returning null). This mirrors
// GenerateBoxSampleDownsampleFragmentShader's
// DeclareFragmentEntryPoint(ss, 0, 1, ...) which likewise declares
// the texcoord, and the display pixel shaders, which keep an unused
// TEXCOORD0 at reg0 for the same reason. D3D11 tolerated the missing
// input because CreatePixelShader does not validate against the VS.

#ifndef RESOLUTION_SCALE
#  error "Compile with /D RESOLUTION_SCALE=<N>. See TEMPLATE_VARIANTS in tools/regen_d3d_common_dxbc.py."
#endif

Texture2D    samp0    : register(t0);
SamplerState samp0_ss : register(s0);

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
  out float4 o_col0 : SV_Target0)
{
  float3 color = float3(0.0, 0.0, 0.0);
  uint2 base_coords = uint2(v_pos.xy) * uint2(RESOLUTION_SCALE, RESOLUTION_SCALE);
  for (uint offset_x = 0u; offset_x < RESOLUTION_SCALE; offset_x++)
  {
    for (uint offset_y = 0u; offset_y < RESOLUTION_SCALE; offset_y++)
      color += samp0.Load(int3(int2(base_coords + uint2(offset_x, offset_y)), 0)).rgb;
  }
  color /= float(RESOLUTION_SCALE * RESOLUTION_SCALE);
  o_col0 = float4(color, 1.0);
}
