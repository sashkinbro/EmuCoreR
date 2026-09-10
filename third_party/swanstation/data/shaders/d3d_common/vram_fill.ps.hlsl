// Pre-baked VRAM fill pixel shader for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateVRAMFillFragmentShader(wrapped,
// interlaced) in src/core/gpu_hw_shadergen.cpp. Used by
// GPU_HW_D3D12::GetVRAMFillPipeline for the FillVRAM clear path - hit
// on every screen-clear command from the game (some games clear every
// frame, some only on scene transitions; either way the shader runs
// often enough to matter on cold-flips).
//
// Variant axes: PGXP_DEPTH x WRAPPED x INTERLACED = 2 x 2 x 2 = 8 blobs.
//
//   * PGXP_DEPTH: m_pgxp_depth_buffer runtime toggle. Controls how
//     o_depth is written - either u_fill_color.a (legacy depth-from-
//     alpha behaviour) or constant 1.0 (PGXP-depth mode).
//   * WRAPPED:    fill rect wraps past the VRAM edge. The runtime
//     decides this per-call from the destination rect's geometry
//     (IsVRAMFillOversized at the call site).
//   * INTERLACED: skip every-other-row to preserve the field not
//     currently being displayed. Set when the destination overlaps
//     the active scanout region and interlace is active.
//
// First multi-axis pre-bake exercise (vram_copy and vram_write each
// had one axis only). The TEMPLATE_VARIANTS table in
// tools/regen_d3d_common_dxbc.py enumerates all 8 combinations.
//
// No RESOLUTION_SCALE / VRAM_SIZE references in the body for D3D12
// mode - fixYCoord is identity in HLSL (only flips on OpenGL's
// lower-left origin), so the legacy "CONSTANT uint RESOLUTION_SCALE
// = X;" emission that WriteCommonFunctions does would have been
// dead code in the shadergen path. No scale-refactor needed before
// pre-baking this one.

cbuffer UBOBlock : register(b0)
{
  // 36-byte cbuffer. C++ side is GPU_HW::VRAMFillUBOData (gpu_hw.h);
  // the field order here MUST match the struct member order there.
  // float4 u_fill_color forces 16-byte alignment so it lands at
  // offset 16 (uint2+uint2 = 16 bytes preceding); the trailing uint
  // sits at offset 32.
  uint2 u_dst_coords;
  uint2 u_end_coords;
  float4 u_fill_color;
  uint u_interlaced_displayed_field;
};

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
  out float o_depth : SV_Depth,
  out float4 o_col0 : SV_Target0)
{
#if INTERLACED || WRAPPED
  // Native VRAM coord. fixYCoord is identity on D3D12 (top-left
  // origin matches PSX); the OpenGL backend's GLSL flips here.
  uint2 dst_coords = uint2(uint(v_pos.x), uint(v_pos.y));
#endif

#if INTERLACED
  // Throw away the field that isn't currently being scanned out so
  // the FillVRAM clear doesn't wipe live content from the displayed
  // field. The 1-bit XOR is whether this row matches the displayed
  // field LSB.
  if ((dst_coords.y & 1u) == u_interlaced_displayed_field)
    discard;
#endif

#if WRAPPED
  // Reject pixels outside both halves of a wrapped rect. Same wrap-
  // around-rect logic as vram_copy / vram_write: (coord < base) &&
  // (coord >= end) is the "outside both halves" condition.
  if ((dst_coords.x < u_dst_coords.x && dst_coords.x >= u_end_coords.x) ||
      (dst_coords.y < u_dst_coords.y && dst_coords.y >= u_end_coords.y))
  {
    discard;
  }
#endif

  o_col0 = u_fill_color;

#if PGXP_DEPTH
  o_depth = 1.0f;
#else
  // Legacy behaviour: the alpha channel doubles as the mask bit;
  // PSX games clear-fill with alpha=0 and write the framebuffer
  // depth from that, so unused mask bits read back as far-plane
  // depth. PGXP-depth mode skips this and writes 1.0 unconditionally
  // because PGXP computes per-vertex depth from CPU-side projection
  // data.
  o_depth = u_fill_color.a;
#endif
}
