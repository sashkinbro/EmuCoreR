// Copyright 2026 LibretroAdmin
// Licensed under GPLv2+
// Refer to the LICENSE file included.

#pragma once

#include <cstddef>
#include <cstdint>

// Pre-compiled DXBC blobs shared between the D3D11 and D3D12 backends.
//
// Each blob is generated offline by tools/regen_d3d_common_dxbc.py from the
// matching HLSL source under data/shaders/d3d_common/, and checked into
// src/common/d3d_common/embedded_dxbc/. Nothing in the build system
// invokes fxc.exe / D3DCompile - the .inc files are consumed as plain
// C++ arrays. See data/shaders/d3d_common/README.md for the editing
// workflow.
//
// fxc emits DXBC bytecode at the ps_5_0 / vs_5_0 / cs_5_0 / gs_5_0
// targets that both D3D11 (CreatePixelShader from raw bytecode, same
// signature as src/common/display.hlsl.h's pre-baked precedent) and
// D3D12 (ID3D12PipelineState's D3D12_SHADER_BYTECODE aggregate)
// consume identically. The HLSL in data/shaders/d3d_common/ uses no
// API-version-specific constructs - register binding declarations
// (cbuffer at b0, Texture2D at t0, SamplerState at s0) are honoured
// by both backends and live in the DXBC reflection metadata.
//
// This path replaces the runtime HLSL -> DXBC step that used to be
// done in D3D11::ShaderCompiler::CompileShader (the D3DCompile call
// shared between the D3D11 and D3D12 backends). Every shader is now
// pre-baked, so that step - and D3DCompile / the d3dcompiler
// dependency along with it - has been removed entirely; both backends
// consume these .inc blobs and never compile HLSL at runtime.
//
// Mirror of src/common/vulkan/embedded_shaders.h for the Vulkan
// backend. As individual shaders get migrated off the runtime
// ShaderGen path, this header gains a pair of extern declarations
// per blob:
//
//   extern const uint8_t k_<stem>_ps[];
//   extern const size_t k_<stem>_ps_size_bytes;
//
// (or _vs / _cs / _gs depending on stage). Blob variants with /D
// defines get a suffix per the TEMPLATE_VARIANTS table in
// tools/regen_d3d_common_dxbc.py.
//
// Constructing a D3D12_SHADER_BYTECODE at the PSO creation site is a
// direct aggregate-initialiser - no wrapper helper needed:
//
//   D3D12_SHADER_BYTECODE bc = { k_<stem>_ps, k_<stem>_ps_size_bytes };
//
// On D3D11, the matching call is
// ID3D11Device::CreatePixelShader(bc.pShaderBytecode, bc.BytecodeLength,
// nullptr, ...), wrapped by
// D3D11::ShaderCompiler::CreatePixelShader(device, bytecode, size).
//
namespace D3DCommon::EmbeddedShaders {

// Backend-neutral (data, size) aggregate for the pre-baked DXBC
// blobs. Both backends consume the same byte stream but wrap it
// differently at the API hand-off:
//   D3D12: D3D12_SHADER_BYTECODE { pShaderBytecode, BytecodeLength }
//          passed to GraphicsPipelineBuilder::SetPixelShader at PSO
//          creation time.
//   D3D11: ID3D11Device::CreatePixelShader(bytecode, size, nullptr,
//          &out) wrapped by D3D11::ShaderCompiler::CreatePixelShader.
//
// Picker functions below return this struct. Each caller converts
// to the native API type at the call site - we deliberately don't
// pull d3d12.h or d3d11.h into this header.
struct Bytecode {
  const uint8_t* data;
  size_t size;
};

// ---- Batch FS pre-bake pickers -------------------------------------
//
// Each picker covers one slice of the batch FS variant matrix.
// Mirror of the runtime shadergen output for the same slice
// (texture_mode + filter combo). Inputs are the 4 session-level
// flags that the pre-baked variant matrix is indexed by:
//
//   use_dual_source            from the shadergen formula
//                              m_supports_dual_source_blend &&
//                              ((render_mode != TransparencyDisabled
//                                && render_mode != OnlyOpaque) ||
//                               filter != Nearest)
//                              Same bit drives the PSO blend state's
//                              SRC1_* references; pre-compute at the
//                              caller and pass both consumers.
//   multisamples               raw m_multisamples (1 / 2 / 4 / 8 /
//                              16 / 32). The picker collapses to
//                              (multisamples > 1) for the interp
//                              qualifier choice.
//   per_sample_shading         raw m_per_sample_shading. Wins over
//                              the multisamples-driven centroid
//                              qualifier when set.
//   disable_color_perspective  raw m_disable_color_perspective.
//                              Selects the noperspective variant.
//
// The textured-Nearest picker additionally takes lookup_mode (the
// post-Reserved-dedup texture_mode value, 0/1/2/4/5/6 reachable).
// The Reserved_Direct16Bit / Reserved_RawDirect16Bit dedup must be
// applied by the caller before invoking; the picker assumes 3 and
// 7 are unreachable.

// Batch vertex shader picker. Selects from the 2 blobs at
// embedded_dxbc/batch_vertex_vs_{untextured,textured}.inc by the
// TEXTURED axis. Unlike the FS pickers there is no interp / persp /
// multisample axis: the runtime shadergen adds those qualifiers to the
// VS outputs, but D3D fixes interpolation from the PS input signature,
// so fxc emits identical VS DXBC across every interp / persp
// permutation. The returned bytecode also feeds D3D11
// CreateInputLayout for the matching vertex format.
Bytecode PickBatchVertexShader(bool textured);

// Untextured batch FS variant picker. Selects from the 12 blobs at
// embedded_dxbc/batch_untextured_ps_*.inc.
Bytecode PickBatchUntexturedFS(bool use_dual_source, uint32_t multisamples,
                                bool per_sample_shading, bool disable_color_perspective);

// Textured + Nearest-filter batch FS variant picker. Selects from
// the 72 blobs at embedded_dxbc/batch_textured_nearest_ps_*.inc.
Bytecode PickBatchTexturedNearestFS(uint8_t lookup_mode, bool use_dual_source,
                                     uint32_t multisamples, bool per_sample_shading,
                                     bool disable_color_perspective);

// Textured + Bilinear-family batch FS variant picker. Selects from
// the 144 blobs at embedded_dxbc/batch_textured_bilinear_ps_*.inc.
// Same axes as the Nearest picker, plus a `binalpha` boolean that
// chooses between the Bilinear (binalpha=false) and BilinearBinAlpha
// (binalpha=true) enum values of GPUTextureFilter - the two filters
// share a single HLSL template with a BINALPHA -D gate on the
// ialpha quantisation step at the end of FilteredSampleFromVRAM.
//
// 6 (tex_mode) x 2 (binalpha) x 2 (dual) x 3 (interp) x 2 (persp)
// = 144 entries. Double the Nearest count because of the BINALPHA
// axis.
Bytecode PickBatchTexturedBilinearFS(uint8_t lookup_mode, bool binalpha,
                                      bool use_dual_source, uint32_t multisamples,
                                      bool per_sample_shading,
                                      bool disable_color_perspective);

// Textured + JINC2-family batch FS variant picker. Selects from
// the 144 blobs at embedded_dxbc/batch_textured_jinc2_ps_*.inc.
// Identical axis cardinality + signature to the Bilinear picker
// (`binalpha` selects between JINC2 and JINC2BinAlpha enum values
// via the BINALPHA -D macro arm in the shared HLSL template). The
// runtime cost difference is in the FS body, not in the picker:
// JINC2 runs a 16-tap sinc-windowed resampler with anti-ringing
// instead of Bilinear's 4-tap weighted average. The picker just
// indexes a 144-entry table the same way.
//
// 6 (tex_mode) x 2 (binalpha) x 2 (dual) x 3 (interp) x 2 (persp)
// = 144 entries. Same cardinality as Bilinear.
Bytecode PickBatchTexturedJINC2FS(uint8_t lookup_mode, bool binalpha,
                                   bool use_dual_source, uint32_t multisamples,
                                   bool per_sample_shading,
                                   bool disable_color_perspective);

// Textured + xBR-family batch FS variant picker. Selects from the
// 144 blobs at embedded_dxbc/batch_textured_xbr_ps_*.inc. Identical
// signature to the Bilinear / JINC2 pickers (`binalpha` selects
// between xBR and xBRBinAlpha enum values via the BINALPHA -D
// macro arm). The runtime cost difference vs Bilinear / JINC2 is
// in the FS body, not in the picker: xBR samples a 5x5 neighbourhood,
// runs a 4-quadrant blend decision tree on YCbCr distances, and
// applies per-quadrant line-blend special cases.
//
// 6 (tex_mode) x 2 (binalpha) x 2 (dual) x 3 (interp) x 2 (persp)
// = 144 entries.
//
// xBR is the last filter template to be pre-baked. After this
// activation lands, the shadergen-fallback else arm in
// GetBatchPipeline / GetBatchPixelShader becomes unreachable and
// is deleted (along with the tmp_shadergen helper on D3D11).
Bytecode PickBatchTexturedXBRFS(uint8_t lookup_mode, bool binalpha,
                                 bool use_dual_source, uint32_t multisamples,
                                 bool per_sample_shading,
                                 bool disable_color_perspective);

// --------------------------------------------------------------------

// ---- Downsample pre-bake pickers -----------------------------------
//
// Selection helpers for the 4 downsample fragment shader templates.
// Each picker returns a Bytecode struct from a small static table.
// Inputs are the runtime knobs each template depends on:
//
//   adaptive_downsample_blur     no inputs (1 blob covers all)
//   adaptive_downsample_mip      first_pass (bool)
//   adaptive_downsample_composite resolution_scale (uint, power of
//                                  2 in [2, 16] - Adaptive forces
//                                  pow2 via
//                                  GPU_HW::CalculateResolutionScale)
//   box_sample_downsample        resolution_scale (uint, in
//                                  [2, 16] - Box accepts any scale
//                                  >= 2 up to the cap)
//
// Out-of-range resolution_scale on either picker is a contract
// violation; the picker asserts and returns the closest in-range
// variant (clamped to the table index range) to avoid an outright
// crash in release builds. The caller is expected to filter
// resolution_scale through GetDownsampleMode + CalculateResolution
// Scale so only reachable values arrive here.

// Adaptive downsample blur pass. Single variant, no inputs.
Bytecode PickAdaptiveDownsampleBlurFS();

// Adaptive downsample mip-energy pass. 2 variants on the FIRST_PASS
// boolean (mirror of the runtime
// GenerateAdaptiveDownsampleMipFragmentShader(first_pass) parameter).
Bytecode PickAdaptiveDownsampleMipFS(bool first_pass);

// Adaptive downsample composite (resolve) pass. 4 variants on
// resolution_scale ∈ {2, 4, 8, 16}.
Bytecode PickAdaptiveDownsampleCompositeFS(uint32_t resolution_scale);

// Box-filter downsample pass. 15 variants on resolution_scale ∈
// [2, 16]. Box mode doesn't require power-of-2 scaling.
Bytecode PickBoxSampleDownsampleFS(uint32_t resolution_scale);

// --------------------------------------------------------------------
// Non-batch op pickers. These centralise the variant selection for the
// VRAM-ops / copy fragment shaders so both D3D backends share one
// selection point rather than open-coding the same index logic. The
// returned bytecode is consumed identically: D3D11 wraps it via
// ShaderCompiler::CreatePixelShader, D3D12 via a D3D12_SHADER_BYTECODE
// in the PSO. (The D3D12 backend still open-codes these inline at its
// PSO sites today; it can migrate onto these pickers as a later
// no-functional-change dedup.)

// Copy / blit FS. Single variant, no axes.
Bytecode PickCopyFS();

// VRAM fill FS. 8 variants: pgxp_depth x wrapped x interlaced. pgxp is
// the m_pgxp_depth_buffer session flag; wrapped / interlaced are the
// per-fill flags. Mirror of GenerateVRAMFillFragmentShader plus the
// PGXP_DEPTH macro the shadergen baked from m_pgxp_depth.
Bytecode PickVRAMFillFS(bool pgxp_depth, bool wrapped, bool interlaced);

// VRAM readback FS. 6 variants on multisamples ∈ {1, 2, 4, 8, 16, 32};
// the >1 variants unroll the MSAA sample-resolve loop. Out-of-set
// values fall back to the m1 (non-MSAA) variant.
Bytecode PickVRAMReadFS(uint32_t multisamples);

// VRAM write FS. 2 variants on the pgxp_depth (m_pgxp_depth_buffer)
// flag. The shadergen use_ssbo dimension is GLSL/Vulkan-only and
// always false on D3D, so it doesn't appear here.
Bytecode PickVRAMWriteFS(bool pgxp_depth);

// VRAM copy FS. 2 variants on the pgxp_depth (m_pgxp_depth_buffer)
// flag.
Bytecode PickVRAMCopyFS(bool pgxp_depth);

// VRAM update-depth FS. 2 variants on msaa (m_multisamples > 1); the
// MSAA variant binds Texture2DMS and reads SV_SampleIndex.
Bytecode PickVRAMUpdateDepthFS(bool msaa);

// Display / present FS. 54 variants on
// depth_24 x interlace_mode x smooth_chroma x multisamples:
//   - depth_24:       0, 1
//   - interlace_mode: 0, 1, 2 (None / InterleavedFields / SeparateFields)
//   - smooth_chroma:  only present on depth_24 == 1 (the 16-bit body
//                     never touches the chroma helpers, so fxc emits
//                     identical DXBC for c0/c1 at depth_24 == 0;
//                     smooth_chroma is ignored when depth_24 is false)
//   - multisamples:   1, 2, 4, 8, 16, 32 (out-of-set falls back to m1)
// = 3*6 (depth_24=0) + 3*2*6 (depth_24=1) = 18 + 36 = 54. Mirror of
// GenerateDisplayFragmentShader; smooth_chroma is the caller's
// (depth_24 && m_chroma_smoothing), multisamples is m_multisamples.
Bytecode PickDisplayFS(bool depth_24, uint32_t interlace_mode, bool smooth_chroma, uint32_t multisamples);

// --------------------------------------------------------------------

// Fullscreen-quad vertex shader. Emits a fullscreen triangle in NDC
// from SV_VertexID 0..2 via the standard bit-shift trick - equivalent
// to ShaderGen::GenerateScreenQuadVertexShader() in D3D12 mode. Used
// by every non-batch pipeline in the D3D12 backend (vram_fill /
// vram_copy / vram_write / vram_update_depth / vram_readback /
// display / copy). Zero state dependency, so a single pre-baked
// variant covers all call sites.
//
// Source: data/shaders/d3d_common/fullscreen_quad.vs.hlsl
extern const uint8_t k_fullscreen_quad_vs[];
extern const size_t k_fullscreen_quad_vs_size_bytes;

// UV-quad vertex shader. Equivalent to
// ShaderGen::GenerateUVQuadVertexShader(). Same fullscreen-triangle
// base as the screen / fullscreen quad VS, but remaps the output
// texcoord into the [u_uv_min, u_uv_max] sub-rect from a 16-byte VS
// cbuffer at b0. Single variant. Consumed by the D3D11 adaptive-
// downsample mip-chain pass.
//
// Note: the SCREEN-quad VS (ShaderGen::GenerateScreenQuadVertexShader)
// has no dedicated blob - on D3D it is byte-equivalent to
// k_fullscreen_quad_vs above (identical SV_VertexID bit-shift triangle;
// the shadergen's only extra is a GL-only v_pos.y flip that is dead on
// D3D), so consumers reuse k_fullscreen_quad_vs for it.
//
// Source: data/shaders/d3d_common/uv_quad.vs.hlsl
extern const uint8_t k_uv_quad_vs[];
extern const size_t k_uv_quad_vs_size_bytes;

// Batch vertex shader. Equivalent to
// GPU_HW_ShaderGen::GenerateBatchVertexShader(textured). Two variants
// on the TEXTURED axis only - the textured form adds the
// a_texcoord / a_texpage / a_uv_limits inputs and the v_tex0 /
// v_texpage / v_uv_limits outputs. RESOLUTION_SCALE / PGXP_DEPTH /
// UV_LIMITS are cbuffer-routed so they don't multiply the matrix, and
// the interp / noperspective qualifiers the runtime shadergen adds to
// the VS outputs don't affect the DXBC (D3D fixes interpolation from
// the PS input signature), so there is no interp / persp axis here -
// see PickBatchVertexShader. Consumed by both backends: the textured
// blob also feeds D3D11 CreateInputLayout.
//
// Source: data/shaders/d3d_common/batch_vertex.vs.hlsl
extern const uint8_t k_batch_vertex_vs_untextured[];
extern const size_t k_batch_vertex_vs_untextured_size_bytes;
extern const uint8_t k_batch_vertex_vs_textured[];
extern const size_t k_batch_vertex_vs_textured_size_bytes;

// Copy/blit pixel shader. Equivalent to
// ShaderGen::GenerateCopyFragmentShader() in D3D12 mode. Single
// texture sample with a cbuffer-driven source rect; zero state
// dependency (no MULTISAMPLING split, no RESOLUTION_SCALE bake-in,
// no preprocessor variant axes). Used by GetCopyPipeline for full-
// frame blits (presentation copies, downscaling).
//
// Source: data/shaders/d3d_common/copy.ps.hlsl
extern const uint8_t k_copy_ps[];
extern const size_t k_copy_ps_size_bytes;

// VRAM-to-VRAM copy pixel shader. Equivalent to
// GPU_HW_ShaderGen::GenerateVRAMCopyFragmentShader() in D3D12 mode.
// Used by GPU_HW_D3D12::GetVRAMCopyPipeline for the VRAM-to-VRAM
// copy fast path (sprite caching, text rendering, menu-system blits).
// Two variants on the PGXP_DEPTH on/off axis - select between them
// based on the runtime m_pgxp_depth_buffer state. The cbuffer carries
// u_resolution_scale (post-e56d4d4 refactor), so the same DXBC serves
// every resolution-scale value.
//
// Source: data/shaders/d3d_common/vram_copy.ps.hlsl
extern const uint8_t k_vram_copy_ps_pgxp0[];
extern const size_t k_vram_copy_ps_pgxp0_size_bytes;
extern const uint8_t k_vram_copy_ps_pgxp1[];
extern const size_t k_vram_copy_ps_pgxp1_size_bytes;

// VRAM write pixel shader. Equivalent to
// GPU_HW_ShaderGen::GenerateVRAMWriteFragmentShader(false) in D3D12
// mode. Used by GPU_HW_D3D12::GetVRAMWritePipeline for the CPU->VRAM
// upload path - hit constantly on every game's framebuffer streaming
// path (pre-rendered backgrounds, FMV staging, sprite-page reloads).
// Two variants on PGXP_DEPTH; the shadergen's `use_ssbo` parameter is
// GLSL/Vulkan-only, so for D3D12 only the texture-buffer source path
// gets pre-baked. u_resolution_scale lives in the cbuffer (post-
// 9d2b49d), so one blob per PGXP value serves every resolution scale.
//
// Source: data/shaders/d3d_common/vram_write.ps.hlsl
extern const uint8_t k_vram_write_ps_pgxp0[];
extern const size_t k_vram_write_ps_pgxp0_size_bytes;
extern const uint8_t k_vram_write_ps_pgxp1[];
extern const size_t k_vram_write_ps_pgxp1_size_bytes;

// VRAM fill pixel shader. Equivalent to
// GPU_HW_ShaderGen::GenerateVRAMFillFragmentShader(wrapped, interlaced)
// in D3D12 mode. Used by GPU_HW_D3D12::GetVRAMFillPipeline for the
// screen-clear / FillVRAM path. 8 variants total, indexed by the
// product of PGXP_DEPTH x WRAPPED x INTERLACED at PSO selection time;
// the variant suffix encodes those three bits as p{0,1}w{0,1}i{0,1}.
// The runtime call site builds a 2 x 2 x 2 indexed lookup table
// from these externs and picks the right blob using
// (m_pgxp_depth_buffer, wrapped, interlaced).
//
// No u_resolution_scale dependency in the shader body for D3D12
// (fixYCoord is identity on HLSL; only the OpenGL backend's
// shadergen body references VRAM_SIZE here for the y-flip). So no
// scale-refactor was needed before pre-baking.
//
// Source: data/shaders/d3d_common/vram_fill.ps.hlsl
extern const uint8_t k_vram_fill_ps_p0w0i0[];
extern const size_t k_vram_fill_ps_p0w0i0_size_bytes;
extern const uint8_t k_vram_fill_ps_p0w0i1[];
extern const size_t k_vram_fill_ps_p0w0i1_size_bytes;
extern const uint8_t k_vram_fill_ps_p0w1i0[];
extern const size_t k_vram_fill_ps_p0w1i0_size_bytes;
extern const uint8_t k_vram_fill_ps_p0w1i1[];
extern const size_t k_vram_fill_ps_p0w1i1_size_bytes;
extern const uint8_t k_vram_fill_ps_p1w0i0[];
extern const size_t k_vram_fill_ps_p1w0i0_size_bytes;
extern const uint8_t k_vram_fill_ps_p1w0i1[];
extern const size_t k_vram_fill_ps_p1w0i1_size_bytes;
extern const uint8_t k_vram_fill_ps_p1w1i0[];
extern const size_t k_vram_fill_ps_p1w1i0_size_bytes;
extern const uint8_t k_vram_fill_ps_p1w1i1[];
extern const size_t k_vram_fill_ps_p1w1i1_size_bytes;

// VRAM update-depth pixel shader. Equivalent to
// GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader() in D3D12
// mode. Used by GPU_HW_D3D12::GetVRAMUpdateDepthPipeline for the
// depth-only pass that propagates colour-texture alpha (the PSX
// mask bit) into the depth buffer after every CPU->VRAM upload.
//
// First MSAA texture-binding variant. The msaa0 / msaa1 blobs
// differ in the binding type (Texture2D vs Texture2DMS<float4>),
// the body Load form, and the presence of SV_SampleIndex in the
// entry-point signature. Runtime selection uses m_multisamples > 1
// (same predicate as the shadergen UsingMSAA() helper) so the
// shader's per-sample-shading expectation matches the PSO's MSAA
// configuration.
//
// Source: data/shaders/d3d_common/vram_update_depth.ps.hlsl
extern const uint8_t k_vram_update_depth_ps_msaa0[];
extern const size_t k_vram_update_depth_ps_msaa0_size_bytes;
extern const uint8_t k_vram_update_depth_ps_msaa1[];
extern const size_t k_vram_update_depth_ps_msaa1_size_bytes;

// VRAM read pixel shader. Equivalent to
// GPU_HW_ShaderGen::GenerateVRAMReadFragmentShader() in D3D12
// mode post-2980961 cbuffer routing. Used by
// GPU_HW_D3D12::GetVRAMReadbackPipeline for the upscaled-VRAM-
// to-native-16bpp encode pass that backs screenshot capture,
// libretro readback, and save-state staging.
//
// First MSAA-count cardinality variant. Six values of
// MULTISAMPLES (1, 2, 4, 8, 16, 32). m1 takes the Texture2D
// path with a single Load; m2..m32 use Texture2DMS<float4>
// with an [unroll] loop over the sample count - each value
// produces a different DXBC because the unroll count varies.
// Body size scales linearly with the unroll: m1 = 3.6 KB,
// m32 = 12.6 KB. Total embedded bytecode footprint ~ 38 KB
// across the 6 blobs.
//
// Runtime selection in GetVRAMReadbackPipeline uses a switch
// on m_multisamples - values outside {1, 2, 4, 8, 16, 32}
// shouldn't occur in practice (GPU drivers only expose
// power-of-2 MSAA counts as having quality levels > 0, and
// the UI dropdown is restricted to those values) but the
// switch falls back to the m1 blob with a warn log if one
// ever does.
//
// Source: data/shaders/d3d_common/vram_read.ps.hlsl
extern const uint8_t k_vram_read_ps_m1[];
extern const size_t k_vram_read_ps_m1_size_bytes;
extern const uint8_t k_vram_read_ps_m2[];
extern const size_t k_vram_read_ps_m2_size_bytes;
extern const uint8_t k_vram_read_ps_m4[];
extern const size_t k_vram_read_ps_m4_size_bytes;
extern const uint8_t k_vram_read_ps_m8[];
extern const size_t k_vram_read_ps_m8_size_bytes;
extern const uint8_t k_vram_read_ps_m16[];
extern const size_t k_vram_read_ps_m16_size_bytes;
extern const uint8_t k_vram_read_ps_m32[];
extern const size_t k_vram_read_ps_m32_size_bytes;

// Display pixel shader. Equivalent to
// GPU_HW_ShaderGen::GenerateDisplayFragmentShader(depth_24bit,
// interlace_mode, smooth_chroma) in D3D12 mode post-e64fc28
// cbuffer routing. Used by GPU_HW_D3D12::GetDisplayPipeline for
// the every-frame scanout pass - the per-frame hot shader.
//
// 54 variants total across 4 axes:
//   DEPTH_24BIT (2) x interlace_mode (3) x SMOOTH_CHROMA (2,
//   collapsed to 1 when DEPTH_24BIT=0 since chroma is dead code
//   there) x MULTISAMPLES (6: 1/2/4/8/16/32).
//
// Variant suffix convention: d{0,1}i{0,1,2}c{0,1}m{01,02,04,08,
// 16,32}. interlace_mode maps to the (INTERLACED, INTERLEAVED)
// bool pair: i0=(0,0), i1=(1,1), i2=(1,0). The d=0 set has 18
// entries (no chroma dim); d=1 has 36 (both chroma values).
//
// Runtime selection in GetDisplayPipeline picks via two arrays:
//   k_display_d0[interlace_mode][ms_idx]                  (3 x 6)
//   k_display_d1[interlace_mode][smooth_chroma][ms_idx]   (3 x 2 x 6)
// so the chroma dimension is structurally absent on the d=0
// path. ms_idx is log2(m_multisamples), or 0 fallback for
// unexpected values (the libretro UI dropdown only exposes
// power-of-2 values and the GPU-driver capability detection
// only reports power-of-2 counts as supported, so the fallback
// shouldn't fire in practice).
//
// Source: data/shaders/d3d_common/display.ps.hlsl
extern const uint8_t k_display_ps_d0i0c0m01[];
extern const size_t k_display_ps_d0i0c0m01_size_bytes;
extern const uint8_t k_display_ps_d0i0c0m02[];
extern const size_t k_display_ps_d0i0c0m02_size_bytes;
extern const uint8_t k_display_ps_d0i0c0m04[];
extern const size_t k_display_ps_d0i0c0m04_size_bytes;
extern const uint8_t k_display_ps_d0i0c0m08[];
extern const size_t k_display_ps_d0i0c0m08_size_bytes;
extern const uint8_t k_display_ps_d0i0c0m16[];
extern const size_t k_display_ps_d0i0c0m16_size_bytes;
extern const uint8_t k_display_ps_d0i0c0m32[];
extern const size_t k_display_ps_d0i0c0m32_size_bytes;
extern const uint8_t k_display_ps_d0i1c0m01[];
extern const size_t k_display_ps_d0i1c0m01_size_bytes;
extern const uint8_t k_display_ps_d0i1c0m02[];
extern const size_t k_display_ps_d0i1c0m02_size_bytes;
extern const uint8_t k_display_ps_d0i1c0m04[];
extern const size_t k_display_ps_d0i1c0m04_size_bytes;
extern const uint8_t k_display_ps_d0i1c0m08[];
extern const size_t k_display_ps_d0i1c0m08_size_bytes;
extern const uint8_t k_display_ps_d0i1c0m16[];
extern const size_t k_display_ps_d0i1c0m16_size_bytes;
extern const uint8_t k_display_ps_d0i1c0m32[];
extern const size_t k_display_ps_d0i1c0m32_size_bytes;
extern const uint8_t k_display_ps_d0i2c0m01[];
extern const size_t k_display_ps_d0i2c0m01_size_bytes;
extern const uint8_t k_display_ps_d0i2c0m02[];
extern const size_t k_display_ps_d0i2c0m02_size_bytes;
extern const uint8_t k_display_ps_d0i2c0m04[];
extern const size_t k_display_ps_d0i2c0m04_size_bytes;
extern const uint8_t k_display_ps_d0i2c0m08[];
extern const size_t k_display_ps_d0i2c0m08_size_bytes;
extern const uint8_t k_display_ps_d0i2c0m16[];
extern const size_t k_display_ps_d0i2c0m16_size_bytes;
extern const uint8_t k_display_ps_d0i2c0m32[];
extern const size_t k_display_ps_d0i2c0m32_size_bytes;
extern const uint8_t k_display_ps_d1i0c0m01[];
extern const size_t k_display_ps_d1i0c0m01_size_bytes;
extern const uint8_t k_display_ps_d1i0c0m02[];
extern const size_t k_display_ps_d1i0c0m02_size_bytes;
extern const uint8_t k_display_ps_d1i0c0m04[];
extern const size_t k_display_ps_d1i0c0m04_size_bytes;
extern const uint8_t k_display_ps_d1i0c0m08[];
extern const size_t k_display_ps_d1i0c0m08_size_bytes;
extern const uint8_t k_display_ps_d1i0c0m16[];
extern const size_t k_display_ps_d1i0c0m16_size_bytes;
extern const uint8_t k_display_ps_d1i0c0m32[];
extern const size_t k_display_ps_d1i0c0m32_size_bytes;
extern const uint8_t k_display_ps_d1i0c1m01[];
extern const size_t k_display_ps_d1i0c1m01_size_bytes;
extern const uint8_t k_display_ps_d1i0c1m02[];
extern const size_t k_display_ps_d1i0c1m02_size_bytes;
extern const uint8_t k_display_ps_d1i0c1m04[];
extern const size_t k_display_ps_d1i0c1m04_size_bytes;
extern const uint8_t k_display_ps_d1i0c1m08[];
extern const size_t k_display_ps_d1i0c1m08_size_bytes;
extern const uint8_t k_display_ps_d1i0c1m16[];
extern const size_t k_display_ps_d1i0c1m16_size_bytes;
extern const uint8_t k_display_ps_d1i0c1m32[];
extern const size_t k_display_ps_d1i0c1m32_size_bytes;
extern const uint8_t k_display_ps_d1i1c0m01[];
extern const size_t k_display_ps_d1i1c0m01_size_bytes;
extern const uint8_t k_display_ps_d1i1c0m02[];
extern const size_t k_display_ps_d1i1c0m02_size_bytes;
extern const uint8_t k_display_ps_d1i1c0m04[];
extern const size_t k_display_ps_d1i1c0m04_size_bytes;
extern const uint8_t k_display_ps_d1i1c0m08[];
extern const size_t k_display_ps_d1i1c0m08_size_bytes;
extern const uint8_t k_display_ps_d1i1c0m16[];
extern const size_t k_display_ps_d1i1c0m16_size_bytes;
extern const uint8_t k_display_ps_d1i1c0m32[];
extern const size_t k_display_ps_d1i1c0m32_size_bytes;
extern const uint8_t k_display_ps_d1i1c1m01[];
extern const size_t k_display_ps_d1i1c1m01_size_bytes;
extern const uint8_t k_display_ps_d1i1c1m02[];
extern const size_t k_display_ps_d1i1c1m02_size_bytes;
extern const uint8_t k_display_ps_d1i1c1m04[];
extern const size_t k_display_ps_d1i1c1m04_size_bytes;
extern const uint8_t k_display_ps_d1i1c1m08[];
extern const size_t k_display_ps_d1i1c1m08_size_bytes;
extern const uint8_t k_display_ps_d1i1c1m16[];
extern const size_t k_display_ps_d1i1c1m16_size_bytes;
extern const uint8_t k_display_ps_d1i1c1m32[];
extern const size_t k_display_ps_d1i1c1m32_size_bytes;
extern const uint8_t k_display_ps_d1i2c0m01[];
extern const size_t k_display_ps_d1i2c0m01_size_bytes;
extern const uint8_t k_display_ps_d1i2c0m02[];
extern const size_t k_display_ps_d1i2c0m02_size_bytes;
extern const uint8_t k_display_ps_d1i2c0m04[];
extern const size_t k_display_ps_d1i2c0m04_size_bytes;
extern const uint8_t k_display_ps_d1i2c0m08[];
extern const size_t k_display_ps_d1i2c0m08_size_bytes;
extern const uint8_t k_display_ps_d1i2c0m16[];
extern const size_t k_display_ps_d1i2c0m16_size_bytes;
extern const uint8_t k_display_ps_d1i2c0m32[];
extern const size_t k_display_ps_d1i2c0m32_size_bytes;
extern const uint8_t k_display_ps_d1i2c1m01[];
extern const size_t k_display_ps_d1i2c1m01_size_bytes;
extern const uint8_t k_display_ps_d1i2c1m02[];
extern const size_t k_display_ps_d1i2c1m02_size_bytes;
extern const uint8_t k_display_ps_d1i2c1m04[];
extern const size_t k_display_ps_d1i2c1m04_size_bytes;
extern const uint8_t k_display_ps_d1i2c1m08[];
extern const size_t k_display_ps_d1i2c1m08_size_bytes;
extern const uint8_t k_display_ps_d1i2c1m16[];
extern const size_t k_display_ps_d1i2c1m16_size_bytes;
extern const uint8_t k_display_ps_d1i2c1m32[];
extern const size_t k_display_ps_d1i2c1m32_size_bytes;

// Batch fragment shader, untextured slice. Equivalent to
// GPU_HW_ShaderGen::GenerateBatchFragmentShader invoked with
// texture_mode == GPUTextureMode::Disabled. Used by
// GPU_HW_D3D12::GetBatchPipeline's untextured branch in place of
// the runtime shadergen + D3DCompile path for the same slice. First
// batch FS template to be pre-baked; the four textured filter
// templates (Nearest / Bilinear / JINC2 / xBR) land in subsequent
// commits.
//
// Variant axes (4, all `-D` to fxc; post-c532a34 TRANSPARENCY-routing):
//
//   * USE_DUAL_SOURCE (0/1):  controls o_col1 declaration.
//   * INTERP_CENTROID:        emits `centroid` qualifier on inputs.
//   * INTERP_SAMPLE:          emits `sample` qualifier (wins over
//                             INTERP_CENTROID if both set; we never
//                             emit a variant where both are 1).
//   * NOPERSP (0/1):          adds `noperspective` to v_col0.
//
// 2 (dual) x 3 (interp: none/centroid/sample) x 2 (persp) = 12 blobs.
// Was 24 pre-c532a34 - the former TRANSPARENCY axis collapsed onto
// the runtime branch on u_render_mode at the body's premultiply and
// dual_source o_col1 sites. MSAA cardinality does not multiply this
// slice (untextured FS has no LOAD_TEXTURE_MS unroll); the
// interpolation qualifier captures the relevant MSAA contribution
// via INTERP_CENTROID/INTERP_SAMPLE.
//
// Runtime selection at the GetBatchPipeline call site:
//   dual_idx         = use_dual_source ? 1 : 0       (mirror of
//                       shadergen formula: m_supports_dual_source_blend &&
//                       ((transparency != Disabled && transparency != OnlyOpaque) ||
//                        m_texture_filter != Nearest))
//   interp_idx       = m_per_sample_shading ? 2 :
//                      (m_multisamples > 1 ? 1 : 0)
//   persp_idx        = m_disable_color_perspective ? 1 : 0
//
// Source: data/shaders/d3d_common/batch_untextured.ps.hlsl
extern const uint8_t k_batch_untextured_ps_d0_none_p0[];
extern const size_t k_batch_untextured_ps_d0_none_p0_size_bytes;
extern const uint8_t k_batch_untextured_ps_d0_none_p1[];
extern const size_t k_batch_untextured_ps_d0_none_p1_size_bytes;
extern const uint8_t k_batch_untextured_ps_d0_centroid_p0[];
extern const size_t k_batch_untextured_ps_d0_centroid_p0_size_bytes;
extern const uint8_t k_batch_untextured_ps_d0_centroid_p1[];
extern const size_t k_batch_untextured_ps_d0_centroid_p1_size_bytes;
extern const uint8_t k_batch_untextured_ps_d0_sample_p0[];
extern const size_t k_batch_untextured_ps_d0_sample_p0_size_bytes;
extern const uint8_t k_batch_untextured_ps_d0_sample_p1[];
extern const size_t k_batch_untextured_ps_d0_sample_p1_size_bytes;
extern const uint8_t k_batch_untextured_ps_d1_none_p0[];
extern const size_t k_batch_untextured_ps_d1_none_p0_size_bytes;
extern const uint8_t k_batch_untextured_ps_d1_none_p1[];
extern const size_t k_batch_untextured_ps_d1_none_p1_size_bytes;
extern const uint8_t k_batch_untextured_ps_d1_centroid_p0[];
extern const size_t k_batch_untextured_ps_d1_centroid_p0_size_bytes;
extern const uint8_t k_batch_untextured_ps_d1_centroid_p1[];
extern const size_t k_batch_untextured_ps_d1_centroid_p1_size_bytes;
extern const uint8_t k_batch_untextured_ps_d1_sample_p0[];
extern const size_t k_batch_untextured_ps_d1_sample_p0_size_bytes;
extern const uint8_t k_batch_untextured_ps_d1_sample_p1[];
extern const size_t k_batch_untextured_ps_d1_sample_p1_size_bytes;

// Batch fragment shader, textured + Nearest-filter slice. Equivalent
// to GPU_HW_ShaderGen::GenerateBatchFragmentShader invoked with
// texture_mode != GPUTextureMode::Disabled AND m_texture_filter ==
// GPUTextureFilter::Nearest. Used by GPU_HW_D3D12::GetBatchPipeline's
// textured-Nearest branch in place of the runtime shadergen +
// D3DCompile path for the same slice. Second batch FS template
// pre-baked, after batch_untextured. The remaining three textured
// filter templates (Bilinear / JINC2 / xBR) land in subsequent
// commits.
//
// Variant axes (4 dimensions, 6+2+3+2 = 72 blobs):
//
//   * Texture mode (6 combos via PALETTE_4_BIT / PALETTE_8_BIT /
//     RAW_TEXTURE -D macros). Palette4Bit / Palette8Bit are mutually
//     exclusive in the shadergen #elif chain - no variant has both
//     set. Reserved_Direct16Bit / Reserved_RawDirect16Bit dedup to
//     Direct16Bit / RawDirect16Bit at the C++ picker level.
//   * USE_DUAL_SOURCE (0/1)
//   * INTERP_CENTROID / INTERP_SAMPLE (mutually exclusive tri-state)
//   * NOPERSP (0/1)
//
// 6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) = 72 blobs.
// MSAA cardinality does not multiply this slice - the batch FS reads
// from the single-sample shadow VRAM regardless of m_multisamples.
//
// Runtime selection at the GetBatchPipeline call site:
//   tm_idx           = derived from lookup_mode (Reserved_* deduped):
//                       0: p0r0 (Direct16Bit)
//                       1: p0r1 (RawDirect16Bit)
//                       2: p4r0 (Palette4Bit)
//                       3: p4r1 (RawPalette4Bit)
//                       4: p8r0 (Palette8Bit)
//                       5: p8r1 (RawPalette8Bit)
//   dual_idx         = use_dual_source ? 1 : 0
//   interp_idx       = m_per_sample_shading ? 2 :
//                      (m_multisamples > 1 ? 1 : 0)
//   persp_idx        = m_disable_color_perspective ? 1 : 0
//
// Source: data/shaders/d3d_common/batch_textured_nearest.ps.hlsl
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d0_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d0_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d0_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d0_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d0_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d0_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d0_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d0_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d0_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d0_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d0_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d0_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d1_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d1_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d1_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d1_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d1_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d1_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d1_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d1_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d1_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d1_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r0_d1_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r0_d1_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d0_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d0_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d0_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d0_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d0_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d0_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d0_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d0_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d0_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d0_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d0_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d0_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d1_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d1_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d1_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d1_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d1_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d1_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d1_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d1_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d1_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d1_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p0r1_d1_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p0r1_d1_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d0_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d0_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d0_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d0_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d0_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d0_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d0_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d0_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d0_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d0_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d0_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d0_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d1_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d1_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d1_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d1_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d1_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d1_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d1_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d1_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d1_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d1_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r0_d1_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r0_d1_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d0_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d0_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d0_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d0_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d0_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d0_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d0_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d0_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d0_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d0_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d0_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d0_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d1_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d1_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d1_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d1_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d1_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d1_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d1_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d1_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d1_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d1_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p4r1_d1_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p4r1_d1_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d0_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d0_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d0_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d0_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d0_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d0_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d0_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d0_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d0_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d0_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d0_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d0_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d1_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d1_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d1_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d1_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d1_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d1_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d1_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d1_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d1_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d1_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r0_d1_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r0_d1_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d0_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d0_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d0_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d0_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d0_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d0_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d0_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d0_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d0_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d0_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d0_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d0_sample_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d1_centroid_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d1_centroid_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d1_centroid_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d1_centroid_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d1_none_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d1_none_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d1_none_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d1_none_n1_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d1_sample_n0[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d1_sample_n0_size_bytes;
extern const uint8_t k_batch_textured_nearest_ps_p8r1_d1_sample_n1[];
extern const size_t k_batch_textured_nearest_ps_p8r1_d1_sample_n1_size_bytes;

// Textured + Bilinear-family batch FS pre-baked DXBC blobs.
// 144 .inc files at src/common/d3d_common/embedded_dxbc/
// batch_textured_bilinear_ps_*.inc.
//
// Variant axes (5, all -D macros to fxc):
//   * 6 texture mode combos (3 -D: PALETTE_4_BIT / PALETTE_8_BIT /
//     RAW_TEXTURE; PALETTE_4_BIT and PALETTE_8_BIT mutually exclusive)
//   * USE_DUAL_SOURCE (0/1)
//   * INTERP_CENTROID / INTERP_SAMPLE (none/centroid/sample tri-state)
//   * NOPERSP (0/1)
//   * BINALPHA (0/1) - new vs Nearest. Gates the ialpha >= 0.5
//     quantisation step in FilteredSampleFromVRAM. BINALPHA=0 =>
//     Bilinear; BINALPHA=1 => BilinearBinAlpha.
//
// 6 x 2 x 3 x 2 x 2 = 144 blobs. Variant suffix:
//   pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_b{0,1}
//
// Foundation commit: 269a2a0. Consumed by PickBatchTexturedBilinearFS.
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p0r1_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p4r1_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_bilinear_ps_p8r1_d1_sample_n1_b1_size_bytes;

// Textured + JINC2-family batch FS pre-baked DXBC blobs.
// 144 .inc files at src/common/d3d_common/embedded_dxbc/
// batch_textured_jinc2_ps_*.inc.
//
// Variant axes (5, all -D macros to fxc) - identical shape to
// the Bilinear family above:
//   * 6 texture mode combos (3 -D: PALETTE_4_BIT / PALETTE_8_BIT /
//     RAW_TEXTURE; PALETTE_4_BIT and PALETTE_8_BIT mutually exclusive)
//   * USE_DUAL_SOURCE (0/1)
//   * INTERP_CENTROID / INTERP_SAMPLE (none/centroid/sample tri-state)
//   * NOPERSP (0/1)
//   * BINALPHA (0/1) - gates the ialpha >= 0.5 quantisation in
//     FilteredSampleFromVRAM. BINALPHA=0 => JINC2; BINALPHA=1 =>
//     JINC2BinAlpha.
//
// 6 x 2 x 3 x 2 x 2 = 144 blobs. Variant suffix:
//   pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_b{0,1}
//
// Per-variant DXBC size is ~3x Bilinear's (17-24 KiB vs ~6 KiB)
// because JINC2's 16 SampleFromVRAM calls + 4x4 weight matrix +
// AR clamp expand the bytecode substantially.
//
// Foundation commit: 17a0c66. Consumed by PickBatchTexturedJINC2FS.
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p0r1_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p4r1_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_jinc2_ps_p8r1_d1_sample_n1_b1_size_bytes;

// Textured + xBR-family batch FS pre-baked DXBC blobs.
// 144 .inc files at src/common/d3d_common/embedded_dxbc/
// batch_textured_xbr_ps_*.inc.
//
// Variant axes (5, all -D macros to fxc) - identical shape to
// the Bilinear / JINC2 families:
//   * 6 texture mode combos (3 -D: PALETTE_4_BIT / PALETTE_8_BIT /
//     RAW_TEXTURE; PALETTE_4_BIT and PALETTE_8_BIT mutually exclusive)
//   * USE_DUAL_SOURCE (0/1)
//   * INTERP_CENTROID / INTERP_SAMPLE (none/centroid/sample tri-state)
//   * NOPERSP (0/1)
//   * BINALPHA (0/1) - gates the ialpha >= 0.5 quantisation in
//     FilteredSampleFromVRAM. BINALPHA=0 => xBR; BINALPHA=1 =>
//     xBRBinAlpha.
//
// 6 x 2 x 3 x 2 x 2 = 144 blobs. Variant suffix:
//   pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_b{0,1}
//
// Per-variant DXBC size is the largest of any pre-baked batch FS
// template (48-60 KiB), because xBR samples a 5x5 neighbourhood,
// runs a 4-quadrant blend decision tree on YCbCr distances, and
// applies per-quadrant line-blend special cases. ~7 MiB embedded
// total. fxc /O3 cannot fold the data-dependent control flow even
// with constant BINALPHA.
//
// Foundation commit: e07ce04. Consumed by PickBatchTexturedXBRFS.
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p0r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p0r1_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p4r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p4r1_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d0_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r0_d1_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r0_d1_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d0_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d0_sample_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_centroid_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_none_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_none_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_none_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_none_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_none_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_none_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_none_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_none_n1_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_sample_n0_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_sample_n0_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_sample_n0_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_sample_n0_b1_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_sample_n1_b0[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_sample_n1_b0_size_bytes;
extern const uint8_t k_batch_textured_xbr_ps_p8r1_d1_sample_n1_b1[];
extern const size_t k_batch_textured_xbr_ps_p8r1_d1_sample_n1_b1_size_bytes;




// Downsample fragment shaders. Backport of the 4 Vulkan downsample
// templates (data/shaders/vulkan/{box_sample_downsample,
// adaptive_downsample_blur, adaptive_downsample_mip,
// adaptive_downsample_composite}.frag.glsl) to HLSL for the D3D
// backends. Consumed today by GPU_HW_D3D11::CompileShaders for
// runtime downsample-pass shader selection in place of the
// shadergen + D3DCompile calls. D3D12 has no downsample
// implementation yet; when phase 2 adds one, it consumes these
// same blobs directly without going through a shadergen path.
//
// Variant axes (4 templates, 22 reachable variants total):
//
//   adaptive_downsample_blur        no axes                1 blob
//   adaptive_downsample_mip         FIRST_PASS (0/1)       2 blobs
//   adaptive_downsample_composite   RESOLUTION_SCALE in    4 blobs
//                                   {2, 4, 8, 16}
//                                   (Adaptive forces pow2
//                                    via CalculateResolutionScale)
//   box_sample_downsample           RESOLUTION_SCALE in    15 blobs
//                                   [2, 16]
//                                   (Box accepts any scale
//                                    >= 2 up to the D3D11/
//                                    D3D12 cap of 16)
//
// Scale=1 disables downsampling entirely (GetDownsampleMode in
// gpu_hw.cpp:399), so no variant covers it.
//
// Source: data/shaders/d3d_common/{adaptive_downsample_blur,
//         adaptive_downsample_mip, adaptive_downsample_composite,
//         box_sample_downsample}.ps.hlsl
extern const uint8_t k_adaptive_downsample_blur_ps[];
extern const size_t k_adaptive_downsample_blur_ps_size_bytes;
extern const uint8_t k_adaptive_downsample_composite_ps_s16[];
extern const size_t k_adaptive_downsample_composite_ps_s16_size_bytes;
extern const uint8_t k_adaptive_downsample_composite_ps_s2[];
extern const size_t k_adaptive_downsample_composite_ps_s2_size_bytes;
extern const uint8_t k_adaptive_downsample_composite_ps_s4[];
extern const size_t k_adaptive_downsample_composite_ps_s4_size_bytes;
extern const uint8_t k_adaptive_downsample_composite_ps_s8[];
extern const size_t k_adaptive_downsample_composite_ps_s8_size_bytes;
extern const uint8_t k_adaptive_downsample_mip_ps_f0[];
extern const size_t k_adaptive_downsample_mip_ps_f0_size_bytes;
extern const uint8_t k_adaptive_downsample_mip_ps_f1[];
extern const size_t k_adaptive_downsample_mip_ps_f1_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s10[];
extern const size_t k_box_sample_downsample_ps_s10_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s11[];
extern const size_t k_box_sample_downsample_ps_s11_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s12[];
extern const size_t k_box_sample_downsample_ps_s12_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s13[];
extern const size_t k_box_sample_downsample_ps_s13_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s14[];
extern const size_t k_box_sample_downsample_ps_s14_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s15[];
extern const size_t k_box_sample_downsample_ps_s15_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s16[];
extern const size_t k_box_sample_downsample_ps_s16_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s2[];
extern const size_t k_box_sample_downsample_ps_s2_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s3[];
extern const size_t k_box_sample_downsample_ps_s3_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s4[];
extern const size_t k_box_sample_downsample_ps_s4_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s5[];
extern const size_t k_box_sample_downsample_ps_s5_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s6[];
extern const size_t k_box_sample_downsample_ps_s6_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s7[];
extern const size_t k_box_sample_downsample_ps_s7_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s8[];
extern const size_t k_box_sample_downsample_ps_s8_size_bytes;
extern const uint8_t k_box_sample_downsample_ps_s9[];
extern const size_t k_box_sample_downsample_ps_s9_size_bytes;



} // namespace D3DCommon::EmbeddedShaders
