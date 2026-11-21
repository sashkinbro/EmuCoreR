// Pre-baked fullscreen-quad vertex shader for the D3D12 backend.
//
// Equivalent to ShaderGen::GenerateScreenQuadVertexShader() invoked with
// HostDisplay::RenderAPI::D3D12 in src/core/shadergen.cpp. The runtime
// path goes through D3DCompile on the generated HLSL; the pre-bake path
// embeds the DXBC output of fxc on this source.
//
// No state dependency. The body computes a fullscreen triangle from
// SV_VertexID (0, 1, 2) using the standard bit-shift trick - v_id 0
// emits (0, 0), v_id 1 emits (2, 0), v_id 2 emits (0, 2) in tex-space,
// mapped to NDC by the * float2(2, -2) + float2(-1, 1) transform.

void main(
  in uint v_id : SV_VertexID,
  out float2 v_tex0 : TEXCOORD0,
  out float4 v_pos : SV_Position)
{
  v_tex0 = float2(float((v_id << 1) & 2u), float(v_id & 2u));
  v_pos = float4(v_tex0 * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}
