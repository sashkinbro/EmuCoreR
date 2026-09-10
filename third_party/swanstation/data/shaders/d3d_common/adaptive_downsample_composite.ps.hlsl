// Pre-baked HLSL for
// GPU_HW_ShaderGen::GenerateAdaptiveDownsampleCompositeFragmentShader().
//
// Backport of data/shaders/vulkan/adaptive_downsample_composite.frag.glsl
// to HLSL for the D3D backends. Originally "mipmap_resolve.glsl"
// from parallel-rsx. Reads the per-pixel mip-bias (single channel)
// computed by the blur pass from samp1, picks a mip level on samp0
// (the original colour pyramid) scaled by (RESOLUTION_SCALE - 1),
// and writes the resolved colour.
//
// Variant axis (1 dimension, 4 blobs):
//   RESOLUTION_SCALE (-D macro, integer literal). Drives:
//     - RCP_VRAM_SIZE = float2(1/1024, 1/512) / RESOLUTION_SCALE
//     - mip = float(RESOLUTION_SCALE - 1) * bias
//   The Vulkan template uses spec constant id=0 for this; on HLSL
//   we use -D RESOLUTION_SCALE=N at fxc time. Reachable values for
//   the Adaptive downsample mode are powers of 2 in
//   [2, m_max_resolution_scale]. The CalculateResolutionScale
//   forces pow2 for Adaptive (gpu_hw.cpp:391). With
//   D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION = 16384 and
//   VRAM_WIDTH = 1024 the cap is 16, so the reachable set is
//   {2, 4, 8, 16} -> 4 variants.
//
// Suffix: s{2,4,8,16}.
//
// Bindings:
//   cbuffer UBOBlock at register(b0): SmoothingUBOData. Not read
//     by this pass but bound by the runtime alongside the other
//     downsample passes.
//   Texture2D samp0 at register(t0)   : colour pyramid (mip 0 =
//                                       upscaled colour at full
//                                       RESOLUTION_SCALE)
//   Texture2D samp1 at register(t1)   : per-pixel mip-bias scalar
//                                       (the blur pass output)
//   SamplerState samp0_ss / samp1_ss at register(s0)/s1
//
// Composite reads v_pos.xy (SV_Position) like the box pass, not
// v_tex0. The arithmetic divides by VRAM_SIZE (1024*scale, 512*
// scale) so the inputs scale identically regardless of the size
// of the rendered triangle.

#ifndef RESOLUTION_SCALE
#  error "Compile with /D RESOLUTION_SCALE=<N>. See TEMPLATE_VARIANTS in tools/regen_d3d_common_dxbc.py."
#endif

cbuffer UBOBlock : register(b0)
{
  float2 u_uv_min;
  float2 u_uv_max;
  float2 u_rcp_resolution;
  float  sample_level;
};

Texture2D    samp0    : register(t0);
SamplerState samp0_ss : register(s0);
Texture2D    samp1    : register(t1);
SamplerState samp1_ss : register(s1);

void main(
  in float4 v_pos : SV_Position,
  out float4 o_col0 : SV_Target0)
{
  // RCP_VRAM_SIZE expansion (same as the WriteCommonFunctions
  // emission for non-batch shaders, with RESOLUTION_SCALE folded
  // to a literal by fxc /D at compile time).
  const float2 rcp_vram_size = float2(1.0 / 1024.0, 1.0 / 512.0) / float(RESOLUTION_SCALE);
  float2 uv = v_pos.xy * rcp_vram_size;
  float bias = samp1.Sample(samp1_ss, uv).r;
  float mip = float(RESOLUTION_SCALE - 1u) * bias;
  float3 color = samp0.SampleLevel(samp0_ss, uv, mip).rgb;
  o_col0 = float4(color, 1.0);
}
