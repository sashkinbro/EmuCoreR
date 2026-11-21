// Pre-baked VRAM-to-VRAM copy pixel shader for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader() in
// src/core/gpu_hw_shadergen.cpp. Used by GPU_HW_D3D12::GetVRAMCopyPipeline
// for the VRAM-to-VRAM copy fast path (sprite caching, text rendering,
// menu-system blits - hit constantly during gameplay).
//
// Variant axis: PGXP_DEPTH = 0 or 1. PGXP-depth disabled / enabled - see
// the runtime m_pgxp_depth_buffer toggle. Two .inc blobs:
//   vram_copy_ps_pgxp0.inc  (PGXP_DEPTH = 0)
//   vram_copy_ps_pgxp1.inc  (PGXP_DEPTH = 1)
//
// RESOLUTION_SCALE / VRAM_SIZE are NOT compile-time baked; they derive
// at runtime from the u_resolution_scale cbuffer field (this is the
// e56d4d4 refactor's payoff). Same DXBC blob serves every resolution
// scale; toggling 1x <-> 4x at runtime is a zero-recompile cbuffer
// write. Same logic as the batch shaders' u_resolution_scale path.
//
// MSAA: vram_copy never runs in MSAA mode (the source texture is bound
// as both shader-resource AND framebuffer-output, which D3D12 / Vulkan
// don't allow with an MSAA texture - see the m_vram_read_texture
// non-MSAA shadow path in each backend). So the MSAA_COPY branch from
// the shadergen output is dead code and is omitted here. The /D
// MSAA_COPY=0 flag is not needed and the shader has no Texture2DMS
// reference at all.

cbuffer UBOBlock : register(b0)
{
  // 48-byte cbuffer. C++ side is GPU_HW::VRAMCopyUBOData (gpu_hw.h);
  // the field order here MUST match the struct member order there.
  uint2 u_src_coords;
  uint2 u_dst_coords;
  uint2 u_end_coords;
  uint2 u_size;
  bool u_set_mask_bit;
  float u_depth_value;
  uint u_resolution_scale;
  uint u_pad0;
};

// VRAM_SIZE derives from u_resolution_scale - mirrors the
// WriteCBufferResolutionScaleAliases helper's #define output in
// gpu_hw_shadergen.cpp. RESOLUTION_SCALE itself is unused in this
// shader's body, so no alias is needed for that name. RCP_VRAM_SIZE
// likewise unused.
#define VRAM_SIZE (uint2(1024u, 512u) * u_resolution_scale)

Texture2D samp0 : register(t0);
SamplerState samp0_ss : register(s0);

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
  out float o_depth : SV_Depth,
  out float4 o_col0 : SV_Target0)
{
  uint2 dst_coords = uint2(v_pos.xy);

  // discard out-of-range pixels - the destination rect may wrap around
  // either VRAM axis. (dst < base) && (dst >= end) means the pixel is
  // outside both halves of a wrapped rect; throw it away.
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }

  // offset of the current pixel from the start of the dest rect, with
  // wrap-around handling on each axis.
  uint2 offset;
  offset.x = (dst_coords.x < u_dst_coords.x)
    ? (VRAM_SIZE.x - u_dst_coords.x + dst_coords.x)
    : (dst_coords.x - u_dst_coords.x);
  offset.y = (dst_coords.y < u_dst_coords.y)
    ? (VRAM_SIZE.y - u_dst_coords.y + dst_coords.y)
    : (dst_coords.y - u_dst_coords.y);

  // map offset back to the source rect (modulo wraps both axes).
  uint2 src_coords = (u_src_coords + offset) % VRAM_SIZE;

  float4 color = samp0.Load(int3(int2(src_coords), 0));

  o_col0 = float4(color.xyz, u_set_mask_bit ? 1.0 : color.a);

#if PGXP_DEPTH
  o_depth = 1.0f;
#else
  o_depth = (u_set_mask_bit ? 1.0f
                            : ((o_col0.a == 1.0) ? u_depth_value : 0.0));
#endif
}
