// Pre-baked copy/blit pixel shader for the D3D12 backend.
//
// Equivalent to ShaderGen::GenerateCopyFragmentShader() in
// src/core/shadergen.cpp under HostDisplay::RenderAPI::D3D12. Used
// by GetCopyPipeline for general full-frame blits (presentation
// copies, downscaling). No state dependency at all - the shader has
// no shadergen prelude, no MULTISAMPLING split, no RESOLUTION_SCALE
// bake-in, and reads its single source rect from the cbuffer at
// runtime. One pre-baked variant covers every caller.

cbuffer UBOBlock : register(b0)
{
  float4 u_src_rect;
};

Texture2D samp0 : register(t0);
SamplerState samp0_ss : register(s0);

void main(
  in float2 v_tex0 : TEXCOORD0,
  out float4 o_col0 : SV_Target0)
{
  float2 coords = u_src_rect.xy + v_tex0 * u_src_rect.zw;
  o_col0 = samp0.Sample(samp0_ss, coords);
}
