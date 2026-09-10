// Pre-baked HLSL for
// GPU_HW_ShaderGen::GenerateAdaptiveDownsampleMipFragmentShader(first_pass).
//
// Backport of data/shaders/vulkan/adaptive_downsample_mip.frag.glsl
// to HLSL for the D3D backends. Originally "mipmap_energy.glsl"
// from parallel-rsx. Measures local "energy" (variance) across a
// 2x2 footprint and writes a scalar bias used downstream to choose
// mip levels.
//
// Variant axis (1 dimension, 2 blobs):
//   FIRST_PASS (-D macro). When 1, the input alpha channel is
//     irrelevant (the source mip 0 has no prior bias to carry) and
//     get_bias operates on float3. When 0 (default), the alpha
//     channel carries the bias from the prior pass and get_bias
//     operates on float4. fxc dead-strips the unused arm at
//     compile time.
//
// This used to be 2 distinct GLSL / HLSL source strings on the
// runtime path - shadergen.Generate*Fragment(first_pass=true) vs
// (false) produced different #define FIRST_PASS sites that hashed
// to different shader-cache entries. The Vulkan pre-bake folded
// these via spec constant id=100; on HLSL we use the equivalent
// -D macro since fxc has no spec-constant mechanism. 2 blobs.
//
// Suffix: f{0,1} encodes FIRST_PASS.
//
// Bindings: same UBO + samp0 as the blur pass. See
// adaptive_downsample_blur.ps.hlsl for the rationale.

cbuffer UBOBlock : register(b0)
{
  float2 u_uv_min;
  float2 u_uv_max;
  float2 u_rcp_resolution;
  float  sample_level;
};

Texture2D    samp0    : register(t0);
SamplerState samp0_ss : register(s0);

// Two overloads of get_bias - the float3 form is for FIRST_PASS=1
// (no prior alpha bias to carry), the float4 form is for
// FIRST_PASS=0 (feed-back).
float4 get_bias(float3 c00, float3 c01, float3 c10, float3 c11)
{
  // Measure the "energy" (variance) in the pixels. If pixels are
  // all the same (2D content), use maximum bias; otherwise taper
  // off quickly back to 0 (edges).
  float3 avg = 0.25 * (c00 + c01 + c10 + c11);
  float s00 = dot(c00 - avg, c00 - avg);
  float s01 = dot(c01 - avg, c01 - avg);
  float s10 = dot(c10 - avg, c10 - avg);
  float s11 = dot(c11 - avg, c11 - avg);
  return float4(avg, 1.0 - log2(1000.0 * (s00 + s01 + s10 + s11) + 1.0));
}

float4 get_bias(float4 c00, float4 c01, float4 c10, float4 c11)
{
  float avg = 0.25 * (c00.a + c01.a + c10.a + c11.a);
  float4 bias = get_bias(c00.rgb, c01.rgb, c10.rgb, c11.rgb);
  bias.a *= avg;
  return bias;
}

void main(
  in float2 v_tex0 : TEXCOORD0,
  out float4 o_col0 : SV_Target0)
{
  float2 uv = v_tex0 - (u_rcp_resolution * 0.25);

#if FIRST_PASS
  float3 c00 = samp0.Sample(samp0_ss, uv, int2(0, 0)).rgb;
  float3 c01 = samp0.Sample(samp0_ss, uv, int2(0, 1)).rgb;
  float3 c10 = samp0.Sample(samp0_ss, uv, int2(1, 0)).rgb;
  float3 c11 = samp0.Sample(samp0_ss, uv, int2(1, 1)).rgb;
  o_col0 = get_bias(c00, c01, c10, c11);
#else
  float4 c00 = samp0.Sample(samp0_ss, uv, int2(0, 0));
  float4 c01 = samp0.Sample(samp0_ss, uv, int2(0, 1));
  float4 c10 = samp0.Sample(samp0_ss, uv, int2(1, 0));
  float4 c11 = samp0.Sample(samp0_ss, uv, int2(1, 1));
  o_col0 = get_bias(c00, c01, c10, c11);
#endif
}
