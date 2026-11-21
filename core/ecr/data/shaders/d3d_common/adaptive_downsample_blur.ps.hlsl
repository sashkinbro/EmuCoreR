// Pre-baked HLSL for
// GPU_HW_ShaderGen::GenerateAdaptiveDownsampleBlurFragmentShader().
//
// Backport of data/shaders/vulkan/adaptive_downsample_blur.frag.glsl
// to HLSL for the D3D backends. Originally "mipmap_blur.glsl" from
// parallel-rsx. Reads a single-channel weight texture, applies a
// 3x3 separable-ish kernel, and writes out a smoothed weight. The
// output is used by the composite pass to choose mip level per
// output pixel.
//
// Variant axes: none. The shader body references neither
// RESOLUTION_SCALE nor any compile-time toggle, so a single
// pre-baked DXBC blob covers every session.
//
// Bindings (matches DeclareUniformBuffer / DeclareTexture in
// shadergen for HLSL, and the D3D11 runtime bind points at
// PSSetConstantBuffers(slot=0) / PSSetShaderResources(slot=0) /
// PSSetSamplers(slot=0)):
//
//   cbuffer UBOBlock at register(b0): GetSmoothingUBO output
//     (u_uv_min, u_uv_max, u_rcp_resolution, sample_level). The
//     sample_level field is unused by this pass but kept in the
//     layout so the same SmoothingUBOData struct serves all three
//     adaptive passes (mip / blur / composite) without a per-pass
//     UBO type.
//   Texture2D samp0 at register(t0)   : prior-pass mip's weight
//                                       channel (alpha)
//   SamplerState samp0_ss at register(s0)

cbuffer UBOBlock : register(b0)
{
  float2 u_uv_min;
  float2 u_uv_max;
  float2 u_rcp_resolution;
  float  sample_level;
};

Texture2D    samp0    : register(t0);
SamplerState samp0_ss : register(s0);

#define UV(x, y) clamp(v_tex0 + float2(x, y) * u_rcp_resolution, u_uv_min, u_uv_max)

void main(
  in float2 v_tex0 : TEXCOORD0,
  out float4 o_col0 : SV_Target0)
{
  // 3x3 weighted kernel on the alpha channel of the prior mip pass.
  // Corner weight 1/16 (w2), edge weight 1/8 (w1), centre weight
  // 1/4 (w0). Sum across 9 taps is 1.0.
  float bias = 0.0;
  const float w0 = 0.25;
  const float w1 = 0.125;
  const float w2 = 0.0625;
  bias += w2 * samp0.Sample(samp0_ss, UV(-1.0, -1.0)).a;
  bias += w2 * samp0.Sample(samp0_ss, UV(+1.0, -1.0)).a;
  bias += w2 * samp0.Sample(samp0_ss, UV(-1.0, +1.0)).a;
  bias += w2 * samp0.Sample(samp0_ss, UV(+1.0, +1.0)).a;
  bias += w1 * samp0.Sample(samp0_ss, UV( 0.0, -1.0)).a;
  bias += w1 * samp0.Sample(samp0_ss, UV(-1.0,  0.0)).a;
  bias += w1 * samp0.Sample(samp0_ss, UV(+1.0,  0.0)).a;
  bias += w1 * samp0.Sample(samp0_ss, UV( 0.0, +1.0)).a;
  bias += w0 * samp0.Sample(samp0_ss, UV( 0.0,  0.0)).a;
  o_col0 = float4(bias, bias, bias, bias);
}
