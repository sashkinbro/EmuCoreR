// Pre-baked batch vertex shader for the D3D11 and D3D12 backends.
//
// Equivalent to GPU_HW_ShaderGen::GenerateBatchVertexShader(textured)
// in src/core/gpu_hw_shadergen.cpp. The runtime path goes through
// D3DCompile on the generated HLSL; the pre-bake path embeds the DXBC
// output of fxc on this source. This is the last shadergen + D3DCompile
// call on the D3D12 backend - once both backends consume the pre-baked
// blobs, D3D12 is fully D3DCompile-free (the non-batch VRAM ops shaders
// and the batch fragment shaders are already pre-baked).
//
// State-invariance. Everything the runtime VS used to bake in at
// compile time is now routed through the 64-byte batch UBO and read as
// a runtime cbuffer value:
//   * RESOLUTION_SCALE -> u_resolution_scale (cbuffer-refactor 7b575a3)
//   * PGXP_DEPTH       -> u_pgxp_depth (uniform-control-flow ternary;
//                         was a compile-time #if PGXP_DEPTH gate)
//   * UV_LIMITS        -> the VS unconditionally declares + writes
//                         a_uv_limits / v_uv_limits when textured; the
//                         FS gates consumption on u_uv_limits at runtime
// so the only remaining compile-time axis is the vertex-format split
// (TEXTURED).
//
// Why only one axis (2 blobs), unlike the batch FS. The runtime
// shadergen calls DeclareVertexEntryPoint with msaa / per_sample /
// disable_color_perspective, which add centroid / sample / noperspective
// qualifiers to the v_col0 / v_tex0 OUTPUTS. In Direct3D the
// interpolation mode is fixed by the PIXEL shader's INPUT signature,
// not the vertex shader's output signature, so fxc emits byte-identical
// VS DXBC regardless of those qualifiers (verified: all 6 interp x
// persp permutations hash identically per TEXTURED value). The matching
// interpolation axes therefore live only on the batch FS templates; the
// VS needs just the textured / untextured split. Both backends already
// keep a [textured] 2-slot batch VS array, so this maps 1:1.
//
// Variant axis (1, -D to fxc):
//
//   * TEXTURED: 0 or 1. Adds the a_texcoord / a_texpage / a_uv_limits
//     vertex inputs (ATTR2..ATTR4) and the v_tex0 / v_texpage /
//     v_uv_limits outputs. Selected at runtime by whether the batch is
//     textured.
//
// Total: 2 (textured) blobs.

#define HLSL 1
#define CONSTANT static const

cbuffer UBOBlock : register(b0)
{
  // 64-byte cbuffer. C++ side is GPU_HW::BatchUBOData (gpu_hw.h:111);
  // field order MUST match the struct member order there and the batch
  // FS templates' identical cbuffer. The VS reads only
  // u_resolution_scale and u_pgxp_depth; the rest are present for layout
  // compatibility (the FS consumes them).
  uint2 u_texture_window_and;
  uint2 u_texture_window_or;
  float u_src_alpha_factor;
  float u_dst_alpha_factor;
  uint  u_interlaced_displayed_field;
  bool  u_set_mask_while_drawing;
  uint  u_resolution_scale;
  uint  u_true_color;
  uint  u_scaled_dithering;
  uint  u_dithering;
  uint  u_interlacing;
  uint  u_pgxp_depth;
  uint  u_uv_limits;
  uint  u_render_mode;
};

#define RESOLUTION_SCALE u_resolution_scale

void main(
  in float4 a_pos : ATTR0,
  in float4 a_col0 : ATTR1,
#if TEXTURED
  in uint a_texcoord : ATTR2,
  in uint a_texpage : ATTR3,
  in float4 a_uv_limits : ATTR4,
#endif
  out float4 v_col0 : COLOR0,
#if TEXTURED
  out float2 v_tex0 : TEXCOORD0,
  nointerpolation out uint4 v_texpage : TEXCOORD1,
  nointerpolation out float4 v_uv_limits : TEXCOORD2,
#endif
  out float4 v_pos : SV_Position)
{
  // Offset the vertex position by 0.5 to ensure correct interpolation of
  // texture coordinates at 1x resolution scale. This doesn't work at
  // >1x, we adjust the texture coordinates before uploading there
  // instead.
  float vertex_offset = (RESOLUTION_SCALE == 1u) ? 0.5 : 0.0;

  // 0..+1023 -> -1..1
  float pos_x = ((a_pos.x + vertex_offset) / 512.0) - 1.0;
  float pos_y = ((a_pos.y + vertex_offset) / -256.0) + 1.0;

  // PGXP-depth mode (u_pgxp_depth != 0) ignores mask Z and uses a_pos.w
  // as the depth source; the legacy path reads a_pos.z. u_pgxp_depth is
  // a cbuffer scalar so this is a uniform-control-flow select - the
  // driver collapses it to a single conditional move.
  float pos_z = (u_pgxp_depth != 0u) ? a_pos.w : a_pos.z;
  float pos_w = a_pos.w;

  v_pos = float4(pos_x * pos_w, pos_y * pos_w, pos_z * pos_w, pos_w);

  v_col0 = a_col0;
#if TEXTURED
  v_tex0 = float2(float((a_texcoord & 0xFFFFu) * RESOLUTION_SCALE),
                  float((a_texcoord >> 16) * RESOLUTION_SCALE));

  // base_x,base_y,palette_x,palette_y
  v_texpage.x = (a_texpage & 15u) * 64u * RESOLUTION_SCALE;
  v_texpage.y = ((a_texpage >> 4) & 1u) * 256u * RESOLUTION_SCALE;
  v_texpage.z = ((a_texpage >> 16) & 63u) * 16u * RESOLUTION_SCALE;
  v_texpage.w = ((a_texpage >> 22) & 511u) * RESOLUTION_SCALE;

  // v_uv_limits is always written when textured. When u_uv_limits=0 at
  // runtime the FS short-circuits and doesn't consume this value, so the
  // (potentially-zero) contents of a_uv_limits are harmless.
  v_uv_limits = a_uv_limits * float4(255.0, 255.0, 255.0, 255.0);
#endif
}
