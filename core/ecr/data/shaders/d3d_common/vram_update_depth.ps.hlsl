// Pre-baked VRAM update-depth pixel shader for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader()
// in src/core/gpu_hw_shadergen.cpp. Used by
// GPU_HW_D3D12::GetVRAMUpdateDepthPipeline for the depth-only pass
// that propagates the colour-texture alpha channel (which doubles
// as the PSX 16-bit "mask" bit) into the depth buffer after VRAM
// uploads. Hit on every CPU->VRAM upload completion path in
// games that toggle the mask bit (most native rendering, and any
// game's transparency-using sprite layer).
//
// Variant axis: MULTISAMPLING = 0 or 1. Two .inc blobs:
//   vram_update_depth_ps_msaa0.inc  (MULTISAMPLING = 0)
//   vram_update_depth_ps_msaa1.inc  (MULTISAMPLING = 1)
//
// First MSAA texture-binding variant exercise. Unlike vram_copy /
// vram_write / vram_fill which only differ in body branches, this
// shader's two variants have different *binding* types:
//
//   MSAA=0: Texture2D samp0           (.Load(int3(coords, mip)) form)
//   MSAA=1: Texture2DMS<float4> samp0 (.Load(coords, sample) form)
//
// SV_SampleIndex is also conditionally declared in the entry point
// for the MSAA path (HLSL forces per-sample shading when a shader
// declares SV_SampleIndex; D3D12 PSO MSAA setting must match the
// shader expectation). The pattern below sets up runtime selection
// in GetVRAMUpdateDepthPipeline using m_multisamples > 1, which is
// the same predicate UsingMSAA() returns in the shadergen path.
//
// This shader has no cbuffer - GenerateVRAMUpdateDepthFragmentShader
// doesn't call DeclareUniformBuffer. v_tex0 is declared but unused
// (the shadergen entry-point template emits texcoord inputs
// unconditionally; matching it here for the input signature alone -
// the optimiser drops the dead read).

#if MULTISAMPLING
  Texture2DMS<float4> samp0 : register(t0);
  SamplerState samp0_ss : register(s0);
#else
  Texture2D samp0 : register(t0);
  SamplerState samp0_ss : register(s0);
#endif

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
#if MULTISAMPLING
  in uint f_sample_index : SV_SampleIndex,
#endif
  out float o_depth : SV_Depth)
{
#if MULTISAMPLING
  // Per-sample load. f_sample_index is declared by the entry-point
  // signature above and ranges 0 .. (samples - 1). HLSL invokes the
  // shader once per sample because SV_SampleIndex is present, so
  // one Load picks the right sample slice every invocation.
  o_depth = samp0.Load(int2(v_pos.xy), f_sample_index).a;
#else
  // Mip 0, point fetch. samp0.Load takes (coords.x, coords.y, mip)
  // packed in an int3 for non-MS Texture2D. No filtering - this is
  // a 1:1 pixel-aligned propagation step, not a resampling pass.
  o_depth = samp0.Load(int3(int2(v_pos.xy), 0)).a;
#endif
}
