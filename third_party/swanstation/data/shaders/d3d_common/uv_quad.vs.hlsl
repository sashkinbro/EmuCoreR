// Pre-baked UV-quad vertex shader for the D3D11 backend.
//
// Equivalent to ShaderGen::GenerateUVQuadVertexShader() invoked with
// HostDisplay::RenderAPI::D3D11 in src/core/shadergen.cpp. The runtime
// path goes through D3DCompile on the generated HLSL; the pre-bake path
// embeds the DXBC output of fxc on this source.
//
// Same fullscreen-triangle base as fullscreen_quad.vs.hlsl (the
// screen-quad VS), but the output texcoord is remapped into the
// [u_uv_min, u_uv_max] sub-rect supplied via the cbuffer. Used by the
// D3D11 adaptive-downsample mip-chain pass (the only UV-quad consumer).
// Single variant - no preprocessor axes - so the regen tool's
// no-variants fallback bakes it to uv_quad_vs.inc / k_uv_quad_vs.
//
// The cbuffer layout MUST match what gpu_hw_d3d11 uploads through the
// VS constant buffer at b0: two float2 (u_uv_min, u_uv_max) packed into
// a single 16-byte register, exactly as ShaderGen::DeclareUniformBuffer
// emits it for D3D.

cbuffer UBOBlock : register(b0)
{
  float2 u_uv_min;
  float2 u_uv_max;
};

void main(
  in uint v_id : SV_VertexID,
  out float2 v_tex0 : TEXCOORD0,
  out float4 v_pos : SV_Position)
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
  v_tex0 = u_uv_min + (u_uv_max - u_uv_min) * v_tex0;
}
