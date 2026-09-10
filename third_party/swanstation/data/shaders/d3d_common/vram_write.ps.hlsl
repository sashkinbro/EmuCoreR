// Pre-baked VRAM write pixel shader for the D3D12 backend.
//
// Equivalent to GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(false)
// in src/core/gpu_hw_shadergen.cpp. Used by GPU_HW_D3D12::GetVRAMWritePipeline
// for the CPU->VRAM upload path - this is hit on every game's
// framebuffer streaming (pre-rendered backgrounds in FF7, FMV decode
// staging, sprite-page reloads in fighting games, etc., so it runs
// many times per frame in practice).
//
// Variant axis: PGXP_DEPTH = 0 or 1. PGXP-depth disabled / enabled -
// the runtime m_pgxp_depth_buffer toggle. Two .inc blobs:
//   vram_write_ps_pgxp0.inc  (PGXP_DEPTH = 0)
//   vram_write_ps_pgxp1.inc  (PGXP_DEPTH = 1)
//
// use_ssbo: the shadergen path takes a `bool use_ssbo` parameter, but
// the D3D12 caller always passes `false` (HLSL has no direct SSBO
// equivalent; the shadergen's use_ssbo=true branch is GLSL/Vulkan-only).
// So the D3D12 pre-bake only covers the texture-buffer path
// (Buffer<uint4> samp0), and the use_ssbo dimension collapses to a
// single value here.
//
// RESOLUTION_SCALE / VRAM_SIZE derive at runtime from u_resolution_scale
// in the cbuffer below (post-9d2b49d refactor). Same DXBC serves
// every resolution-scale value.

cbuffer UBOBlock : register(b0)
{
  // 44-byte cbuffer. C++ side is GPU_HW::VRAMWriteUBOData (gpu_hw.h);
  // the field order here MUST match the struct member order there.
  uint2 u_base_coords;
  uint2 u_end_coords;
  uint2 u_size;
  uint u_buffer_base_offset;
  uint u_mask_or_bits;
  float u_depth_value;
  uint u_resolution_scale;
  uint u_pad0;
};

// VRAM_SIZE / RESOLUTION_SCALE derive from u_resolution_scale - mirrors
// the WriteCBufferResolutionScaleAliases helper's #define output in
// gpu_hw_shadergen.cpp.
#define RESOLUTION_SCALE u_resolution_scale
#define VRAM_SIZE        (uint2(1024u, 512u) * u_resolution_scale)

// Texture-buffer source for the CPU upload data. PSX VRAM writes are
// 16-bit RGBA5551 packed into the texel buffer; the shader does
// per-pixel address arithmetic + RGBA5551 -> RGBA8 conversion below.
// `Buffer<uint4>` here matches what shadergen.cpp's DeclareTextureBuffer
// emits for HLSL when is_int=true / is_unsigned=true.
Buffer<uint4> samp0 : register(t0);

// RGBA5551 -> RGBA8 unpack. PSX 16-bit colour layout (bits 0-4 R,
// 5-9 G, 10-14 B, 15 alpha/mask). Identical to the helper inlined by
// WriteCommonFunctions in the shadergen path.
float4 RGBA5551ToRGBA8(uint v)
{
  uint r = (v & 31u);
  uint g = ((v >> 5) & 31u);
  uint b = ((v >> 10) & 31u);
  uint a = ((v >> 15) & 1u);
  return float4(float(r) / 31.0, float(g) / 31.0, float(b) / 31.0, float(a));
}

void main(
  in float2 v_tex0 : TEXCOORD0,
  in float4 v_pos : SV_Position,
  out float o_depth : SV_Depth,
  out float4 o_col0 : SV_Target0)
{
  // Map upscaled framebuffer pixel back to native VRAM coord. The
  // runtime sets the viewport in upscaled VRAM space; the divide by
  // RESOLUTION_SCALE collapses N upscaled pixels into one native one
  // so the texture-buffer lookup uses native VRAM addressing.
  uint2 coords = uint2(uint(v_pos.x) / RESOLUTION_SCALE,
                       uint(v_pos.y) / RESOLUTION_SCALE);

  // Discard out-of-range pixels. Same wrap-around-rect logic as
  // vram_copy: (coord < base) && (coord >= end) means the pixel sits
  // outside both halves of a wrapped rect.
  if ((coords.x < u_base_coords.x && coords.x >= u_end_coords.x) ||
      (coords.y < u_base_coords.y && coords.y >= u_end_coords.y))
  {
    discard;
  }

  // Offset from the start of the rect, with native-VRAM wrap-around
  // handling on each axis. (VRAM_SIZE.x / RESOLUTION_SCALE) is just
  // 1024 in native coords; written this way to match the shadergen
  // body byte-for-byte so the optimization opportunities are
  // identical.
  uint2 offset;
  offset.x = (coords.x < u_base_coords.x)
    ? ((VRAM_SIZE.x / RESOLUTION_SCALE) - u_base_coords.x + coords.x)
    : (coords.x - u_base_coords.x);
  offset.y = (coords.y < u_base_coords.y)
    ? ((VRAM_SIZE.y / RESOLUTION_SCALE) - u_base_coords.y + coords.y)
    : (coords.y - u_base_coords.y);

  // Linearise (offset.x, offset.y) into a 1-D buffer index, fetch the
  // 16-bit texel from the upload buffer, OR in the mask bit, unpack
  // to RGBA8.
  uint buffer_offset = u_buffer_base_offset + (offset.y * u_size.x) + offset.x;
  uint value = samp0.Load(int(buffer_offset)).r | u_mask_or_bits;

  o_col0 = RGBA5551ToRGBA8(value);

#if PGXP_DEPTH
  o_depth = 1.0;
#else
  o_depth = (o_col0.a == 1.0) ? u_depth_value : 0.0;
#endif
}
