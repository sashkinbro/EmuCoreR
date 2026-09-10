#!/usr/bin/env python3
# Regenerate embedded DXBC blobs from HLSL sources.
#
# This script is NOT invoked by the core build. It is run by contributors who
# edit shader sources under data/shaders/d3d_common/. The build itself only
# sees the resulting .inc files (checked into
# src/common/d3d_common/embedded_dxbc/), so the libretro core has zero
# fxc.exe / D3DCompile dependency at build time.
#
# The .inc files are shared between the D3D11 and D3D12 backends - DXBC at
# ps_5_0 / vs_5_0 etc. is consumed identically by both, and the shadergen
# emits identical HLSL across the API_D3D11 / API_D3D12 macros (the macros
# exist but no batch FS body references them).
#
# Mirror of tools/regen_vulkan_spirv.py for the Vulkan backend. The same
# editing workflow applies: edit .hlsl source -> rerun this script -> commit
# both .hlsl and the regenerated .inc together so the two stay in sync.
#
# Usage:
#     python3 tools/regen_d3d_common_dxbc.py [--fxc PATH] [--wine PATH]
#
# Requires fxc.exe from the Windows 10 SDK. On Linux, requires Wine to run
# fxc.exe under emulation - pass the wine binary via --wine or set up
# `wine fxc.exe` to be invokable through the script's auto-detection. On
# Windows, the script auto-detects fxc.exe in the Windows 10 SDK install
# location (one of the kit / bin / x64 / fxc.exe paths) or via PATH.

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
HLSL_DIR = REPO_ROOT / "data" / "shaders" / "d3d_common"
INC_DIR = REPO_ROOT / "src" / "common" / "d3d_common" / "embedded_dxbc"

# Map HLSL filename suffix to (fxc target prefix, identifier suffix). The
# first element is what we pass to fxc /T (e.g. "ps_5_0"); the second is the
# C++ identifier tail (e.g. k_<stem>_ps). Anything not in this table is
# skipped with a warning. Shader Model 5.0 matches the runtime baseline of
# D3D_FEATURE_LEVEL_11_0 which the D3D12 backend targets via the shared
# D3D11::ShaderCompiler path; see src/common/d3d11/shader_compiler.cpp.
STAGE_FROM_SUFFIX = {
    ".vs.hlsl": ("vs_5_0", "vs"),
    ".ps.hlsl": ("ps_5_0", "ps"),
    ".cs.hlsl": ("cs_5_0", "cs"),
    ".gs.hlsl": ("gs_5_0", "gs"),
}

# Templates: shader sources that produce more than one .inc output, each
# with a different combination of /D preprocessor defines. Index ordering
# here MUST match the helper in src/common/d3d_common/embedded_shaders.cpp that
# picks the right blob at runtime - if you add a variant here, add the
# matching entry there too.
#
# For each entry: filename -> [(variant_suffix, [/D defines]), ...]
#
# Empty for now - this dict grows as batch / per-filter / per-pgxp variants
# get pre-baked. Foundation patches lay the infrastructure; subsequent
# patches add HLSL sources and the corresponding variant tables alongside.
TEMPLATE_VARIANTS = {
    # vram_copy_ps: two variants for the PGXP_DEPTH on/off branch. See
    # GPU_HW_D3D12::GetVRAMCopyPipeline at the runtime call site -
    # `m_pgxp_depth_buffer` picks between blob[0] and blob[1].
    "vram_copy.ps.hlsl": [
        ("pgxp0", ["PGXP_DEPTH=0"]),
        ("pgxp1", ["PGXP_DEPTH=1"]),
    ],
    # vram_write_ps: same PGXP_DEPTH on/off split as vram_copy. The
    # shadergen's `use_ssbo` parameter is GLSL/Vulkan-only - the D3D12
    # call site at GPU_HW_D3D12::GetVRAMWritePipeline always passes
    # false, so the use_ssbo dimension collapses to a single value
    # and doesn't show up here.
    "vram_write.ps.hlsl": [
        ("pgxp0", ["PGXP_DEPTH=0"]),
        ("pgxp1", ["PGXP_DEPTH=1"]),
    ],
    # vram_fill_ps: first multi-axis variant entry. 3 axes
    # (PGXP_DEPTH, WRAPPED, INTERLACED) = 8 blobs. Variant suffix
    # encodes the three flags as p{0,1}w{0,1}i{0,1} so the
    # alphabetical ordering of the .inc files matches the natural
    # nested-loop iteration order; the embedded_shaders.{h,cpp}
    # extern declarations and the GetVRAMFillPipeline call site
    # mirror this naming convention.
    "vram_fill.ps.hlsl": [
        ("p0w0i0", ["PGXP_DEPTH=0", "WRAPPED=0", "INTERLACED=0"]),
        ("p0w0i1", ["PGXP_DEPTH=0", "WRAPPED=0", "INTERLACED=1"]),
        ("p0w1i0", ["PGXP_DEPTH=0", "WRAPPED=1", "INTERLACED=0"]),
        ("p0w1i1", ["PGXP_DEPTH=0", "WRAPPED=1", "INTERLACED=1"]),
        ("p1w0i0", ["PGXP_DEPTH=1", "WRAPPED=0", "INTERLACED=0"]),
        ("p1w0i1", ["PGXP_DEPTH=1", "WRAPPED=0", "INTERLACED=1"]),
        ("p1w1i0", ["PGXP_DEPTH=1", "WRAPPED=1", "INTERLACED=0"]),
        ("p1w1i1", ["PGXP_DEPTH=1", "WRAPPED=1", "INTERLACED=1"]),
    ],
    # vram_update_depth_ps: first MSAA texture-binding variant. Two
    # variants on MULTISAMPLING; unlike the body-branch variants
    # above, the two blobs have different texture *binding* types
    # (Texture2D vs Texture2DMS<float4>) plus a conditional
    # SV_SampleIndex input on the MSAA path. Runtime selection in
    # GetVRAMUpdateDepthPipeline picks between them via
    # m_multisamples > 1 (same predicate as the shadergen
    # UsingMSAA() helper).
    "vram_update_depth.ps.hlsl": [
        ("msaa0", ["MULTISAMPLING=0"]),
        ("msaa1", ["MULTISAMPLING=1"]),
    ],
    # vram_read_ps: MSAA-count cardinality variant. Six values
    # of MULTISAMPLES (1, 2, 4, 8, 16, 32) - the m1 variant uses
    # Texture2D (no MSAA path); m2..m32 all use
    # Texture2DMS<float4> but unroll the LoadVRAM sample-resolve
    # loop a different number of times. Runtime selection in
    # GetVRAMReadbackPipeline picks via switch on m_multisamples.
    # MULTISAMPLING is derived in-shader (#if MULTISAMPLES > 1)
    # rather than passed as a separate /D.
    "vram_read.ps.hlsl": [
        ("m1", ["MULTISAMPLES=1"]),
        ("m2", ["MULTISAMPLES=2"]),
        ("m4", ["MULTISAMPLES=4"]),
        ("m8", ["MULTISAMPLES=8"]),
        ("m16", ["MULTISAMPLES=16"]),
        ("m32", ["MULTISAMPLES=32"]),
    ],
    # display_ps: prize variant set. 4 axes, 54 unique blobs.
    # DEPTH_24BIT (2) x interlace_mode (3) x SMOOTH_CHROMA (2,
    # collapsed to 1 when DEPTH_24BIT=0 since the chroma path is
    # dead code there) x MULTISAMPLES (6).
    # interlace_mode is the InterlacedRenderMode enum, mapped to
    # (INTERLACED, INTERLEAVED) bool pair:
    #   i=0 (None):              (0, 0)
    #   i=1 (InterleavedFields): (1, 1)
    #   i=2 (SeparateFields):    (1, 0)
    # MULTISAMPLES is padded to 2 digits in the suffix so
    # alphabetical sort of .inc filenames matches the natural
    # nested-loop iteration order. Runtime selection in
    # GetDisplayPipeline uses two arrays (k_display_d0 and
    # k_display_d1) so the chroma dimension is genuinely
    # absent on the d=0 path rather than indexed-and-ignored.
    "display.ps.hlsl": [
        ("d0i0c0m01", ["DEPTH_24BIT=0", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=1"]),
        ("d0i0c0m02", ["DEPTH_24BIT=0", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=2"]),
        ("d0i0c0m04", ["DEPTH_24BIT=0", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=4"]),
        ("d0i0c0m08", ["DEPTH_24BIT=0", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=8"]),
        ("d0i0c0m16", ["DEPTH_24BIT=0", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=16"]),
        ("d0i0c0m32", ["DEPTH_24BIT=0", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=32"]),
        ("d0i1c0m01", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=1"]),
        ("d0i1c0m02", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=2"]),
        ("d0i1c0m04", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=4"]),
        ("d0i1c0m08", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=8"]),
        ("d0i1c0m16", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=16"]),
        ("d0i1c0m32", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=32"]),
        ("d0i2c0m01", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=1"]),
        ("d0i2c0m02", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=2"]),
        ("d0i2c0m04", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=4"]),
        ("d0i2c0m08", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=8"]),
        ("d0i2c0m16", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=16"]),
        ("d0i2c0m32", ["DEPTH_24BIT=0", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=32"]),
        ("d1i0c0m01", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=1"]),
        ("d1i0c0m02", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=2"]),
        ("d1i0c0m04", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=4"]),
        ("d1i0c0m08", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=8"]),
        ("d1i0c0m16", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=16"]),
        ("d1i0c0m32", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=32"]),
        ("d1i0c1m01", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=1"]),
        ("d1i0c1m02", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=2"]),
        ("d1i0c1m04", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=4"]),
        ("d1i0c1m08", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=8"]),
        ("d1i0c1m16", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=16"]),
        ("d1i0c1m32", ["DEPTH_24BIT=1", "INTERLACED=0", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=32"]),
        ("d1i1c0m01", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=1"]),
        ("d1i1c0m02", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=2"]),
        ("d1i1c0m04", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=4"]),
        ("d1i1c0m08", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=8"]),
        ("d1i1c0m16", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=16"]),
        ("d1i1c0m32", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=0", "MULTISAMPLES=32"]),
        ("d1i1c1m01", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=1", "MULTISAMPLES=1"]),
        ("d1i1c1m02", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=1", "MULTISAMPLES=2"]),
        ("d1i1c1m04", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=1", "MULTISAMPLES=4"]),
        ("d1i1c1m08", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=1", "MULTISAMPLES=8"]),
        ("d1i1c1m16", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=1", "MULTISAMPLES=16"]),
        ("d1i1c1m32", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=1", "SMOOTH_CHROMA=1", "MULTISAMPLES=32"]),
        ("d1i2c0m01", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=1"]),
        ("d1i2c0m02", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=2"]),
        ("d1i2c0m04", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=4"]),
        ("d1i2c0m08", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=8"]),
        ("d1i2c0m16", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=16"]),
        ("d1i2c0m32", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=0", "MULTISAMPLES=32"]),
        ("d1i2c1m01", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=1"]),
        ("d1i2c1m02", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=2"]),
        ("d1i2c1m04", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=4"]),
        ("d1i2c1m08", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=8"]),
        ("d1i2c1m16", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=16"]),
        ("d1i2c1m32", ["DEPTH_24BIT=1", "INTERLACED=1", "INTERLEAVED=0", "SMOOTH_CHROMA=1", "MULTISAMPLES=32"]),
    ],

    # batch_untextured_ps: first batch FS pre-bake template. Mirrors
    # the C++ shadergen GenerateBatchFragmentShader output for the
    # texture_mode == GPUTextureMode::Disabled slice. 4 structural
    # axes (post-c532a34 TRANSPARENCY routing):
    #
    #   * USE_DUAL_SOURCE (0/1): drives o_col1 declaration + write.
    #   * INTERP_CENTROID / INTERP_SAMPLE: mutually exclusive booleans
    #     encoding the (none / centroid / sample) tri-state. Sample
    #     wins over centroid when both flags happen to be set; we
    #     never emit a variant where both are 1.
    #   * NOPERSP (0/1): `noperspective` qualifier on v_col0.
    #
    # 2 (dual) x 3 (interp) x 2 (persp) = 12 blobs. The former
    # TRANSPARENCY axis (2 values) collapsed onto the runtime branch
    # on u_render_mode in the HLSL body at the premultiply +
    # dual_source o_col1 sites - the DXBC is invariant under
    # render_mode post-routing. MSAA does NOT multiply this slice -
    # the untextured FS body has no LOAD_TEXTURE_MS sample-resolve
    # loop to unroll; MSAA only affects the input qualifier
    # (centroid/sample), which the INTERP_* axes already cover.
    #
    # Variant suffix: d{0,1}_{none,centroid,sample}_p{0,1}.
    # Alphabetical sort matches the natural nested-loop iteration
    # order in the matching helper at embedded_shaders.cpp.
    # batch_vertex_vs: the batch vertex shader, shared by both D3D
    # backends. 1 axis (TEXTURED) = 2 blobs. TEXTURED adds the
    # a_texcoord/a_texpage/a_uv_limits inputs + v_tex0/v_texpage/
    # v_uv_limits outputs. RESOLUTION_SCALE / PGXP_DEPTH / UV_LIMITS are
    # cbuffer-routed so they don't multiply. Unlike the batch FS, there
    # is NO interp/persp axis: the runtime shadergen adds centroid /
    # sample / noperspective qualifiers to the VS outputs, but D3D fixes
    # interpolation from the PS input signature, so fxc emits identical
    # VS DXBC for every interp/persp permutation (verified by hashing).
    # The matching interp axes live on the FS templates only.
    #
    # Variant suffix: untextured / textured. Picker at
    # embedded_shaders.cpp indexes [textured].
    "batch_vertex.vs.hlsl": [
        ("untextured", []),
        ("textured",   ["TEXTURED=1"]),
    ],

    "batch_untextured.ps.hlsl": [
        # dual=0
        ("d0_none_p0",     []),
        ("d0_none_p1",     ["NOPERSP=1"]),
        ("d0_centroid_p0", ["INTERP_CENTROID=1"]),
        ("d0_centroid_p1", ["INTERP_CENTROID=1", "NOPERSP=1"]),
        ("d0_sample_p0",   ["INTERP_SAMPLE=1"]),
        ("d0_sample_p1",   ["INTERP_SAMPLE=1", "NOPERSP=1"]),
        # dual=1
        ("d1_none_p0",     ["USE_DUAL_SOURCE=1"]),
        ("d1_none_p1",     ["USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("d1_centroid_p0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("d1_centroid_p1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("d1_sample_p0",   ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("d1_sample_p1",   ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
    ],

    # batch_textured_nearest_ps: second batch FS pre-bake template.
    # Mirror of the C++ shadergen GenerateBatchFragmentShader output
    # for the (texture_mode != Disabled, m_texture_filter == Nearest)
    # slice. 4 structural axes (post-c532a34 TRANSPARENCY routing):
    #
    #   * Texture mode (6 combos, 3 -D macros):
    #     (PALETTE_4_BIT, PALETTE_8_BIT, RAW_TEXTURE) tuple. Valid:
    #       (0, 0, 0): Direct16Bit
    #       (0, 0, 1): RawDirect16Bit
    #       (1, 0, 0): Palette4Bit
    #       (1, 0, 1): RawPalette4Bit
    #       (0, 1, 0): Palette8Bit
    #       (0, 1, 1): RawPalette8Bit
    #     PALETTE_4_BIT and PALETTE_8_BIT are mutually exclusive; no
    #     variant has both set. The Reserved_Direct16Bit /
    #     Reserved_RawDirect16Bit enum values fold to their non-
    #     Reserved counterparts at the C++ picker level.
    #   * USE_DUAL_SOURCE (0/1): drives o_col1 declaration + write.
    #   * INTERP_CENTROID / INTERP_SAMPLE: mutually exclusive
    #     booleans encoding the (none / centroid / sample) tri-state.
    #     Sample wins over centroid when both flags happen to be set.
    #   * NOPERSP (0/1): `noperspective` qualifier on v_col0.
    #
    # 6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) = 72 blobs.
    # The MSAA axis does NOT multiply this template - the batch FS
    # samples from the single-sample shadow VRAM regardless of
    # m_multisamples; MSAA only affects the interp qualifier which
    # the INTERP_* axes already cover.
    #
    # Variant suffix: pXrY_d{0,1}_{none,centroid,sample}_n{0,1}
    # where:
    #   pX: palette mode (X = 4 for Palette4Bit, 8 for Palette8Bit,
    #                     0 for non-palette / Direct16Bit)
    #   rY: RawTextureBit (Y = 0 or 1)
    #   dZ: USE_DUAL_SOURCE bit (Z = 0 or 1)
    #   nW: NOPERSP bit (W = 0 or 1)
    # Alphabetical sort matches the natural nested-loop iteration
    # order in the matching helper at gpu_hw_d3d12.cpp.
    "batch_textured_nearest.ps.hlsl": [
        ("p0r0_d0_none_n0", []),
        ("p0r0_d0_none_n1", ["NOPERSP=1"]),
        ("p0r0_d0_centroid_n0", ["INTERP_CENTROID=1"]),
        ("p0r0_d0_centroid_n1", ["INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d0_sample_n0", ["INTERP_SAMPLE=1"]),
        ("p0r0_d0_sample_n1", ["INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d1_none_n0", ["USE_DUAL_SOURCE=1"]),
        ("p0r0_d1_none_n1", ["USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r0_d1_centroid_n0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r0_d1_centroid_n1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d1_sample_n0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r0_d1_sample_n1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d0_none_n0", ["RAW_TEXTURE=1"]),
        ("p0r1_d0_none_n1", ["RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p0r1_d0_centroid_n0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d0_centroid_n1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d0_sample_n0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d0_sample_n1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d1_none_n0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p0r1_d1_none_n1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r1_d1_centroid_n0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d1_centroid_n1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d1_sample_n0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d1_sample_n1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d0_none_n0", ["PALETTE_4_BIT=1"]),
        ("p4r0_d0_none_n1", ["PALETTE_4_BIT=1", "NOPERSP=1"]),
        ("p4r0_d0_centroid_n0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1"]),
        ("p4r0_d0_centroid_n1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d0_sample_n0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d0_sample_n1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d1_none_n0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p4r0_d1_none_n1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r0_d1_centroid_n0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r0_d1_centroid_n1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d1_sample_n0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d1_sample_n1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d0_none_n0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1"]),
        ("p4r1_d0_none_n1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p4r1_d0_centroid_n0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d0_centroid_n1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d0_sample_n0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d0_sample_n1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d1_none_n0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p4r1_d1_none_n1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r1_d1_centroid_n0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d1_centroid_n1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d1_sample_n0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d1_sample_n1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d0_none_n0", ["PALETTE_8_BIT=1"]),
        ("p8r0_d0_none_n1", ["PALETTE_8_BIT=1", "NOPERSP=1"]),
        ("p8r0_d0_centroid_n0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1"]),
        ("p8r0_d0_centroid_n1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d0_sample_n0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d0_sample_n1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d1_none_n0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p8r0_d1_none_n1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r0_d1_centroid_n0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r0_d1_centroid_n1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d1_sample_n0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d1_sample_n1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d0_none_n0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1"]),
        ("p8r1_d0_none_n1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p8r1_d0_centroid_n0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d0_centroid_n1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d0_sample_n0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d0_sample_n1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d1_none_n0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p8r1_d1_none_n1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r1_d1_centroid_n0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d1_centroid_n1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d1_sample_n0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d1_sample_n1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
    ],

    # batch_textured_bilinear_ps: third batch FS pre-bake template.
    # Mirror of the C++ shadergen GenerateBatchFragmentShader output
    # for the (texture_mode != Disabled, m_texture_filter in
    # {Bilinear, BilinearBinAlpha}) slice. 5 structural axes:
    #
    #   * Texture mode (6 combos via 3 -D macros):
    #     Same encoding as batch_textured_nearest - see that entry
    #     for the (PALETTE_4_BIT, PALETTE_8_BIT, RAW_TEXTURE) tuple
    #     mapping. PALETTE_4_BIT and PALETTE_8_BIT are mutually
    #     exclusive.
    #   * USE_DUAL_SOURCE (0/1): drives o_col1 declaration + write.
    #   * INTERP_CENTROID / INTERP_SAMPLE: interp tri-state.
    #   * NOPERSP (0/1): noperspective on v_col0.
    #   * BINALPHA (0/1): NEW axis vs Nearest. Gates the ialpha
    #     quantisation step at the end of FilteredSampleFromVRAM
    #     (BINALPHA=1 => BilinearBinAlpha; BINALPHA=0 => standard
    #     Bilinear). fxc dead-strips the unused arm.
    #
    # 6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) x 2 (BinAlpha)
    # = 144 blobs. Double the Nearest count (72) because BINALPHA
    # adds one dimension. MSAA cardinality does NOT multiply this
    # template either - the batch FS reads the single-sample shadow
    # VRAM regardless of m_multisamples.
    #
    # Variant suffix: pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_b{0,1}
    # where b{0,1} encodes BINALPHA. Alphabetical sort matches the
    # natural nested-loop iteration order in the matching helper.
    "batch_textured_bilinear.ps.hlsl": [
        ("p0r0_d0_none_n0_b0", []),
        ("p0r0_d0_none_n0_b1", ["BINALPHA=1"]),
        ("p0r0_d0_none_n1_b0", ["NOPERSP=1"]),
        ("p0r0_d0_none_n1_b1", ["NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d0_centroid_n0_b0", ["INTERP_CENTROID=1"]),
        ("p0r0_d0_centroid_n0_b1", ["INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r0_d0_centroid_n1_b0", ["INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d0_centroid_n1_b1", ["INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d0_sample_n0_b0", ["INTERP_SAMPLE=1"]),
        ("p0r0_d0_sample_n0_b1", ["INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r0_d0_sample_n1_b0", ["INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d0_sample_n1_b1", ["INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_none_n0_b0", ["USE_DUAL_SOURCE=1"]),
        ("p0r0_d1_none_n0_b1", ["USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p0r0_d1_none_n1_b0", ["USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r0_d1_none_n1_b1", ["USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_centroid_n0_b0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r0_d1_centroid_n0_b1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r0_d1_centroid_n1_b0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d1_centroid_n1_b1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_sample_n0_b0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r0_d1_sample_n0_b1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r0_d1_sample_n1_b0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d1_sample_n1_b1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_none_n0_b0", ["RAW_TEXTURE=1"]),
        ("p0r1_d0_none_n0_b1", ["RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p0r1_d0_none_n1_b0", ["RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p0r1_d0_none_n1_b1", ["RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_centroid_n0_b0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d0_centroid_n0_b1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r1_d0_centroid_n1_b0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d0_centroid_n1_b1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_sample_n0_b0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d0_sample_n0_b1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r1_d0_sample_n1_b0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d0_sample_n1_b1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_none_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p0r1_d1_none_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p0r1_d1_none_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r1_d1_none_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_centroid_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d1_centroid_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r1_d1_centroid_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d1_centroid_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_sample_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d1_sample_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r1_d1_sample_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d1_sample_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_none_n0_b0", ["PALETTE_4_BIT=1"]),
        ("p4r0_d0_none_n0_b1", ["PALETTE_4_BIT=1", "BINALPHA=1"]),
        ("p4r0_d0_none_n1_b0", ["PALETTE_4_BIT=1", "NOPERSP=1"]),
        ("p4r0_d0_none_n1_b1", ["PALETTE_4_BIT=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_centroid_n0_b0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1"]),
        ("p4r0_d0_centroid_n0_b1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r0_d0_centroid_n1_b0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d0_centroid_n1_b1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_sample_n0_b0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d0_sample_n0_b1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r0_d0_sample_n1_b0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d0_sample_n1_b1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_none_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p4r0_d1_none_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p4r0_d1_none_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r0_d1_none_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_centroid_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r0_d1_centroid_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r0_d1_centroid_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d1_centroid_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_sample_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d1_sample_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r0_d1_sample_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d1_sample_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_none_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1"]),
        ("p4r1_d0_none_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p4r1_d0_none_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p4r1_d0_none_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_centroid_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d0_centroid_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r1_d0_centroid_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d0_centroid_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_sample_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d0_sample_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r1_d0_sample_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d0_sample_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_none_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p4r1_d1_none_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p4r1_d1_none_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r1_d1_none_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_centroid_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d1_centroid_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r1_d1_centroid_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d1_centroid_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_sample_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d1_sample_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r1_d1_sample_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d1_sample_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_none_n0_b0", ["PALETTE_8_BIT=1"]),
        ("p8r0_d0_none_n0_b1", ["PALETTE_8_BIT=1", "BINALPHA=1"]),
        ("p8r0_d0_none_n1_b0", ["PALETTE_8_BIT=1", "NOPERSP=1"]),
        ("p8r0_d0_none_n1_b1", ["PALETTE_8_BIT=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_centroid_n0_b0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1"]),
        ("p8r0_d0_centroid_n0_b1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r0_d0_centroid_n1_b0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d0_centroid_n1_b1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_sample_n0_b0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d0_sample_n0_b1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r0_d0_sample_n1_b0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d0_sample_n1_b1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_none_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p8r0_d1_none_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p8r0_d1_none_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r0_d1_none_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_centroid_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r0_d1_centroid_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r0_d1_centroid_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d1_centroid_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_sample_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d1_sample_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r0_d1_sample_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d1_sample_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_none_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1"]),
        ("p8r1_d0_none_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p8r1_d0_none_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p8r1_d0_none_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_centroid_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d0_centroid_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r1_d0_centroid_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d0_centroid_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_sample_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d0_sample_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r1_d0_sample_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d0_sample_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_none_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p8r1_d1_none_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p8r1_d1_none_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r1_d1_none_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_centroid_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d1_centroid_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r1_d1_centroid_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d1_centroid_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_sample_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d1_sample_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r1_d1_sample_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d1_sample_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
    ],


    # batch_textured_jinc2_ps: fourth batch FS pre-bake template.
    # Mirror of the C++ shadergen output for the JINC2 / JINC2BinAlpha
    # filter family. Identical axis cardinality to Bilinear:
    #
    #   6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) x 2 (BinAlpha)
    #   = 144 blobs.
    #
    # The (PALETTE_4_BIT, PALETTE_8_BIT, RAW_TEXTURE) tuple encoding,
    # USE_DUAL_SOURCE / INTERP_* / NOPERSP / BINALPHA -D macros, and
    # variant suffix scheme (pXrY_d{0,1}_{none,centroid,sample}_n{0,1}_
    # b{0,1}) are identical to batch_textured_bilinear.ps.hlsl. Only
    # the FilteredSampleFromVRAM body differs:
    #
    #   * 16 SampleFromVRAM corner taps over a 4x4 footprint
    #   * Sinc-windowed kernel (jinc2_resampler) with JINC2_WA / WB
    #     constants
    #   * Anti-ringing clamp + lerp at JINC2_AR_STRENGTH=0.8
    #   * BINALPHA quantises the resampler-weighted alpha to {0, 1}
    #
    # Per-variant DXBC is larger than Bilinear (~17-24 KiB vs ~6 KiB
    # for Bilinear) because the 16-tap + resampler math expands the
    # bytecode substantially. 144 x ~20 KiB = ~3 MiB on disk.
    "batch_textured_jinc2.ps.hlsl": [
        ("p0r0_d0_none_n0_b0", []),
        ("p0r0_d0_none_n0_b1", ["BINALPHA=1"]),
        ("p0r0_d0_none_n1_b0", ["NOPERSP=1"]),
        ("p0r0_d0_none_n1_b1", ["NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d0_centroid_n0_b0", ["INTERP_CENTROID=1"]),
        ("p0r0_d0_centroid_n0_b1", ["INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r0_d0_centroid_n1_b0", ["INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d0_centroid_n1_b1", ["INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d0_sample_n0_b0", ["INTERP_SAMPLE=1"]),
        ("p0r0_d0_sample_n0_b1", ["INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r0_d0_sample_n1_b0", ["INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d0_sample_n1_b1", ["INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_none_n0_b0", ["USE_DUAL_SOURCE=1"]),
        ("p0r0_d1_none_n0_b1", ["USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p0r0_d1_none_n1_b0", ["USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r0_d1_none_n1_b1", ["USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_centroid_n0_b0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r0_d1_centroid_n0_b1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r0_d1_centroid_n1_b0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d1_centroid_n1_b1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_sample_n0_b0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r0_d1_sample_n0_b1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r0_d1_sample_n1_b0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d1_sample_n1_b1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_none_n0_b0", ["RAW_TEXTURE=1"]),
        ("p0r1_d0_none_n0_b1", ["RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p0r1_d0_none_n1_b0", ["RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p0r1_d0_none_n1_b1", ["RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_centroid_n0_b0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d0_centroid_n0_b1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r1_d0_centroid_n1_b0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d0_centroid_n1_b1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_sample_n0_b0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d0_sample_n0_b1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r1_d0_sample_n1_b0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d0_sample_n1_b1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_none_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p0r1_d1_none_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p0r1_d1_none_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r1_d1_none_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_centroid_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d1_centroid_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r1_d1_centroid_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d1_centroid_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_sample_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d1_sample_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r1_d1_sample_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d1_sample_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_none_n0_b0", ["PALETTE_4_BIT=1"]),
        ("p4r0_d0_none_n0_b1", ["PALETTE_4_BIT=1", "BINALPHA=1"]),
        ("p4r0_d0_none_n1_b0", ["PALETTE_4_BIT=1", "NOPERSP=1"]),
        ("p4r0_d0_none_n1_b1", ["PALETTE_4_BIT=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_centroid_n0_b0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1"]),
        ("p4r0_d0_centroid_n0_b1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r0_d0_centroid_n1_b0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d0_centroid_n1_b1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_sample_n0_b0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d0_sample_n0_b1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r0_d0_sample_n1_b0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d0_sample_n1_b1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_none_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p4r0_d1_none_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p4r0_d1_none_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r0_d1_none_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_centroid_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r0_d1_centroid_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r0_d1_centroid_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d1_centroid_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_sample_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d1_sample_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r0_d1_sample_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d1_sample_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_none_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1"]),
        ("p4r1_d0_none_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p4r1_d0_none_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p4r1_d0_none_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_centroid_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d0_centroid_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r1_d0_centroid_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d0_centroid_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_sample_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d0_sample_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r1_d0_sample_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d0_sample_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_none_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p4r1_d1_none_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p4r1_d1_none_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r1_d1_none_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_centroid_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d1_centroid_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r1_d1_centroid_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d1_centroid_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_sample_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d1_sample_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r1_d1_sample_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d1_sample_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_none_n0_b0", ["PALETTE_8_BIT=1"]),
        ("p8r0_d0_none_n0_b1", ["PALETTE_8_BIT=1", "BINALPHA=1"]),
        ("p8r0_d0_none_n1_b0", ["PALETTE_8_BIT=1", "NOPERSP=1"]),
        ("p8r0_d0_none_n1_b1", ["PALETTE_8_BIT=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_centroid_n0_b0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1"]),
        ("p8r0_d0_centroid_n0_b1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r0_d0_centroid_n1_b0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d0_centroid_n1_b1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_sample_n0_b0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d0_sample_n0_b1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r0_d0_sample_n1_b0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d0_sample_n1_b1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_none_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p8r0_d1_none_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p8r0_d1_none_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r0_d1_none_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_centroid_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r0_d1_centroid_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r0_d1_centroid_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d1_centroid_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_sample_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d1_sample_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r0_d1_sample_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d1_sample_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_none_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1"]),
        ("p8r1_d0_none_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p8r1_d0_none_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p8r1_d0_none_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_centroid_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d0_centroid_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r1_d0_centroid_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d0_centroid_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_sample_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d0_sample_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r1_d0_sample_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d0_sample_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_none_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p8r1_d1_none_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p8r1_d1_none_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r1_d1_none_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_centroid_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d1_centroid_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r1_d1_centroid_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d1_centroid_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_sample_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d1_sample_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r1_d1_sample_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d1_sample_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
    ],

    # batch_textured_xbr_ps: fifth and final batch FS pre-bake
    # template. Mirror of the C++ shadergen output for the xBR /
    # xBRBinAlpha filter family. Identical axis cardinality to
    # Bilinear / JINC2:
    #
    #   6 (tex_mode) x 2 (dual) x 3 (interp) x 2 (persp) x 2 (BinAlpha)
    #   = 144 blobs.
    #
    # Variant suffix scheme identical: pXrY_d{0,1}_{none,centroid,
    # sample}_n{0,1}_b{0,1}.
    #
    # Only the FilteredSampleFromVRAM body differs. xBR samples a
    # 5x5 neighbourhood, computes YCbCr distances between adjacent
    # samples, decides per-quadrant blend at three levels
    # (BLEND_NONE / BLEND_NORMAL / BLEND_DOMINANT) with separate
    # line-blend special cases per quadrant. Per-variant DXBC is
    # ~48-60 KiB (~2x JINC2's, ~10x Bilinear's). 144 x ~55 KiB =
    # ~8 MiB on disk.
    "batch_textured_xbr.ps.hlsl": [
        ("p0r0_d0_none_n0_b0", []),
        ("p0r0_d0_none_n0_b1", ["BINALPHA=1"]),
        ("p0r0_d0_none_n1_b0", ["NOPERSP=1"]),
        ("p0r0_d0_none_n1_b1", ["NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d0_centroid_n0_b0", ["INTERP_CENTROID=1"]),
        ("p0r0_d0_centroid_n0_b1", ["INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r0_d0_centroid_n1_b0", ["INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d0_centroid_n1_b1", ["INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d0_sample_n0_b0", ["INTERP_SAMPLE=1"]),
        ("p0r0_d0_sample_n0_b1", ["INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r0_d0_sample_n1_b0", ["INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d0_sample_n1_b1", ["INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_none_n0_b0", ["USE_DUAL_SOURCE=1"]),
        ("p0r0_d1_none_n0_b1", ["USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p0r0_d1_none_n1_b0", ["USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r0_d1_none_n1_b1", ["USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_centroid_n0_b0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r0_d1_centroid_n0_b1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r0_d1_centroid_n1_b0", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r0_d1_centroid_n1_b1", ["USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r0_d1_sample_n0_b0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r0_d1_sample_n0_b1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r0_d1_sample_n1_b0", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r0_d1_sample_n1_b1", ["USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_none_n0_b0", ["RAW_TEXTURE=1"]),
        ("p0r1_d0_none_n0_b1", ["RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p0r1_d0_none_n1_b0", ["RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p0r1_d0_none_n1_b1", ["RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_centroid_n0_b0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d0_centroid_n0_b1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r1_d0_centroid_n1_b0", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d0_centroid_n1_b1", ["RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d0_sample_n0_b0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d0_sample_n0_b1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r1_d0_sample_n1_b0", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d0_sample_n1_b1", ["RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_none_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p0r1_d1_none_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p0r1_d1_none_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p0r1_d1_none_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_centroid_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p0r1_d1_centroid_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p0r1_d1_centroid_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p0r1_d1_centroid_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p0r1_d1_sample_n0_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p0r1_d1_sample_n0_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p0r1_d1_sample_n1_b0", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p0r1_d1_sample_n1_b1", ["RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_none_n0_b0", ["PALETTE_4_BIT=1"]),
        ("p4r0_d0_none_n0_b1", ["PALETTE_4_BIT=1", "BINALPHA=1"]),
        ("p4r0_d0_none_n1_b0", ["PALETTE_4_BIT=1", "NOPERSP=1"]),
        ("p4r0_d0_none_n1_b1", ["PALETTE_4_BIT=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_centroid_n0_b0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1"]),
        ("p4r0_d0_centroid_n0_b1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r0_d0_centroid_n1_b0", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d0_centroid_n1_b1", ["PALETTE_4_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d0_sample_n0_b0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d0_sample_n0_b1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r0_d0_sample_n1_b0", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d0_sample_n1_b1", ["PALETTE_4_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_none_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p4r0_d1_none_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p4r0_d1_none_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r0_d1_none_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_centroid_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r0_d1_centroid_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r0_d1_centroid_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r0_d1_centroid_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r0_d1_sample_n0_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r0_d1_sample_n0_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r0_d1_sample_n1_b0", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r0_d1_sample_n1_b1", ["PALETTE_4_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_none_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1"]),
        ("p4r1_d0_none_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p4r1_d0_none_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p4r1_d0_none_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_centroid_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d0_centroid_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r1_d0_centroid_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d0_centroid_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d0_sample_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d0_sample_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r1_d0_sample_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d0_sample_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_none_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p4r1_d1_none_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p4r1_d1_none_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p4r1_d1_none_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_centroid_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p4r1_d1_centroid_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p4r1_d1_centroid_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p4r1_d1_centroid_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p4r1_d1_sample_n0_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p4r1_d1_sample_n0_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p4r1_d1_sample_n1_b0", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p4r1_d1_sample_n1_b1", ["PALETTE_4_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_none_n0_b0", ["PALETTE_8_BIT=1"]),
        ("p8r0_d0_none_n0_b1", ["PALETTE_8_BIT=1", "BINALPHA=1"]),
        ("p8r0_d0_none_n1_b0", ["PALETTE_8_BIT=1", "NOPERSP=1"]),
        ("p8r0_d0_none_n1_b1", ["PALETTE_8_BIT=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_centroid_n0_b0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1"]),
        ("p8r0_d0_centroid_n0_b1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r0_d0_centroid_n1_b0", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d0_centroid_n1_b1", ["PALETTE_8_BIT=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d0_sample_n0_b0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d0_sample_n0_b1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r0_d0_sample_n1_b0", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d0_sample_n1_b1", ["PALETTE_8_BIT=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_none_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1"]),
        ("p8r0_d1_none_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p8r0_d1_none_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r0_d1_none_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_centroid_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r0_d1_centroid_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r0_d1_centroid_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r0_d1_centroid_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r0_d1_sample_n0_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r0_d1_sample_n0_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r0_d1_sample_n1_b0", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r0_d1_sample_n1_b1", ["PALETTE_8_BIT=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_none_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1"]),
        ("p8r1_d0_none_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "BINALPHA=1"]),
        ("p8r1_d0_none_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1"]),
        ("p8r1_d0_none_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_centroid_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d0_centroid_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r1_d0_centroid_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d0_centroid_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d0_sample_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d0_sample_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r1_d0_sample_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d0_sample_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_none_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1"]),
        ("p8r1_d1_none_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "BINALPHA=1"]),
        ("p8r1_d1_none_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1"]),
        ("p8r1_d1_none_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_centroid_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1"]),
        ("p8r1_d1_centroid_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "BINALPHA=1"]),
        ("p8r1_d1_centroid_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1"]),
        ("p8r1_d1_centroid_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_CENTROID=1", "NOPERSP=1", "BINALPHA=1"]),
        ("p8r1_d1_sample_n0_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1"]),
        ("p8r1_d1_sample_n0_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "BINALPHA=1"]),
        ("p8r1_d1_sample_n1_b0", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1"]),
        ("p8r1_d1_sample_n1_b1", ["PALETTE_8_BIT=1", "RAW_TEXTURE=1", "USE_DUAL_SOURCE=1", "INTERP_SAMPLE=1", "NOPERSP=1", "BINALPHA=1"]),
    ],

    # adaptive_downsample_blur.ps.hlsl is NOT listed here: no variant
    # axes, so the script's no-variants fallback (compile once with
    # no /D defines, emit a single .inc with the base identifier)
    # produces the right output.

    # Adaptive downsample mip pass (mipmap_energy.glsl from
    # parallel-rsx, ported to HLSL). One -D axis:
    #   FIRST_PASS (0/1): float3 vs float4 sampling at the 2x2
    #     footprint. The Vulkan template uses spec constant id=100;
    #     HLSL has no spec-constant equivalent so we use a -D macro
    #     instead and fxc dead-strips the unused arm.
    # Suffix: f{0,1}.
    "adaptive_downsample_mip.ps.hlsl": [
        ("f0", []),
        ("f1", ["FIRST_PASS=1"]),
    ],

    # Adaptive downsample composite pass (mipmap_resolve.glsl from
    # parallel-rsx, ported to HLSL). One -D axis:
    #   RESOLUTION_SCALE: integer literal. Drives RCP_VRAM_SIZE
    #     (= float2(1/1024, 1/512) / RESOLUTION_SCALE) and the mip
    #     multiplier (RESOLUTION_SCALE - 1). The Adaptive
    #     downsample path forces RESOLUTION_SCALE to a power of 2
    #     in [2, m_max_resolution_scale] (gpu_hw.cpp:391); the cap
    #     on D3D11/D3D12 is 16 (D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION
    #     / VRAM_WIDTH = 16384 / 1024), so the reachable set is
    #     {2, 4, 8, 16} - 4 variants. The Vulkan side uses spec
    #     constant id=0; we use -D RESOLUTION_SCALE=N on HLSL.
    # Suffix: s{2,4,8,16}.
    "adaptive_downsample_composite.ps.hlsl": [
        ("s2",  ["RESOLUTION_SCALE=2"]),
        ("s4",  ["RESOLUTION_SCALE=4"]),
        ("s8",  ["RESOLUTION_SCALE=8"]),
        ("s16", ["RESOLUTION_SCALE=16"]),
    ],

    # Box downsample pass. One -D axis:
    #   RESOLUTION_SCALE: integer literal. Drives the base_coords
    #     scaling, the two inner loop bounds, and the averaging
    #     divisor. The Box downsample mode accepts any
    #     RESOLUTION_SCALE in [2, m_max_resolution_scale] (no
    #     power-of-2 constraint, unlike Adaptive); the cap is 16,
    #     so the reachable set is {2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
    #     12, 13, 14, 15, 16} - 15 variants. Scale=1 disables
    #     downsampling entirely (GetDownsampleMode in
    #     gpu_hw.cpp:399 returns Disabled), so we don't bake a
    #     variant for it. The Vulkan side uses spec constant id=0;
    #     we use -D RESOLUTION_SCALE=N on HLSL.
    # Suffix: s{2..16}.
    "box_sample_downsample.ps.hlsl": [
        ("s2",  ["RESOLUTION_SCALE=2"]),
        ("s3",  ["RESOLUTION_SCALE=3"]),
        ("s4",  ["RESOLUTION_SCALE=4"]),
        ("s5",  ["RESOLUTION_SCALE=5"]),
        ("s6",  ["RESOLUTION_SCALE=6"]),
        ("s7",  ["RESOLUTION_SCALE=7"]),
        ("s8",  ["RESOLUTION_SCALE=8"]),
        ("s9",  ["RESOLUTION_SCALE=9"]),
        ("s10", ["RESOLUTION_SCALE=10"]),
        ("s11", ["RESOLUTION_SCALE=11"]),
        ("s12", ["RESOLUTION_SCALE=12"]),
        ("s13", ["RESOLUTION_SCALE=13"]),
        ("s14", ["RESOLUTION_SCALE=14"]),
        ("s15", ["RESOLUTION_SCALE=15"]),
        ("s16", ["RESOLUTION_SCALE=16"]),
    ],
}


def find_fxc(explicit_fxc, explicit_wine):
    # On Windows, look for fxc.exe in the Windows 10 / 11 SDK install
    # location first, then PATH. On Linux, look in any Wine prefix
    # we can find that has the SDK installed; only fall back to the
    # --fxc override when the user explicitly points us at one.
    #
    # Microsoft's actual fxc.exe is required - Wine's bundled
    # d3dcompiler_47.dll (which is the vkd3d-shader reimplementation)
    # produces DXBC that doesn't pass Microsoft D3D12 PSO validation
    # for non-trivial shaders. cd971cd / da20f5b chronicles a
    # vram_copy_ps pre-bake attempt that failed with
    # CreateGraphicsPipelineState E_FAIL because the .inc was built
    # with vkd3d-shader. The check_microsoft_creator() validator
    # below rejects vkd3d-compiled output post-hoc, but it's better
    # to find Microsoft's fxc.exe up-front and skip the wasted
    # compile time entirely. See data/shaders/d3d_common/README.md.
    if explicit_fxc:
        fxc = Path(explicit_fxc)
        if not fxc.exists():
            sys.stderr.write(f"error: --fxc path does not exist: {fxc}\n")
            sys.exit(1)
        return ([str(fxc)] if sys.platform == "win32" else
                [explicit_wine or "wine", str(fxc)])

    # PATH lookup (Windows: fxc.exe; Linux/macOS: unusual but supported).
    for name in ("fxc.exe", "fxc"):
        path = shutil.which(name)
        if path:
            return [path]

    # SDK auto-detection. The Windows 10 / 11 SDK installs fxc.exe at
    # <SDK_root>/<version>/x64/fxc.exe. Try the standard install
    # locations on Windows AND under every plausible Wine prefix on
    # Linux. The /Program Files/ and /Program Files (x86)/ split
    # depends on whether the SDK was installed as 32-bit or 64-bit;
    # check both. Wine prefix discovery covers $WINEPREFIX (if set),
    # the default ~/.wine, and any prefixes under ~/.local/share/
    # wineprefixes/ that PlayOnLinux and lutris-style installs use.
    sdk_roots = [
        Path("C:/Program Files (x86)/Windows Kits/10/bin"),
        Path("C:/Program Files/Windows Kits/10/bin"),
    ]
    wine_prefixes = []
    if "WINEPREFIX" in os.environ:
        wine_prefixes.append(Path(os.environ["WINEPREFIX"]))
    default_prefix = Path.home() / ".wine"
    if default_prefix.exists() and default_prefix not in wine_prefixes:
        wine_prefixes.append(default_prefix)
    extra_prefix_dir = Path.home() / ".local" / "share" / "wineprefixes"
    if extra_prefix_dir.exists():
        for child in extra_prefix_dir.iterdir():
            if child.is_dir() and (child / "drive_c").exists():
                wine_prefixes.append(child)
    for prefix in wine_prefixes:
        sdk_roots.append(prefix / "drive_c" / "Program Files (x86)" / "Windows Kits" / "10" / "bin")
        sdk_roots.append(prefix / "drive_c" / "Program Files" / "Windows Kits" / "10" / "bin")

    for root in sdk_roots:
        if not root.exists():
            continue
        # SDK layout: <root>/<version>/x64/fxc.exe - pick the highest version.
        versions = sorted([p for p in root.iterdir() if p.is_dir() and p.name[0].isdigit()],
                          reverse=True)
        for v in versions:
            candidate = v / "x64" / "fxc.exe"
            if candidate.exists():
                return ([str(candidate)] if sys.platform == "win32" else
                        [explicit_wine or "wine", str(candidate)])

    sys.stderr.write(
        "error: fxc.exe not found. The Microsoft Windows 10/11 SDK ships\n"
        "       it under <SDK>/bin/<version>/x64/fxc.exe. See\n"
        "       data/shaders/d3d_common/README.md for installation steps,\n"
        "       including how to set up the SDK under a Wine prefix\n"
        "       for Linux contributors. If you have fxc.exe elsewhere,\n"
        "       pass --fxc PATH. On Linux pass --wine PATH if `wine`\n"
        "       isn't on PATH.\n"
        "\n"
        "       Wine's bundled d3dcompiler_47.dll (vkd3d-shader\n"
        "       reimplementation) is NOT a substitute - its DXBC output\n"
        "       fails Microsoft D3D12 PSO validation. See da20f5b for\n"
        "       a worked example.\n")
    sys.exit(1)


def extract_rdef_creator(data):
    # Inspect the DXBC RDEF chunk and pull out the compiler-creator
    # string. The DXBC container is:
    #     [0:4]     magic "DXBC"
    #     [4:20]    16-byte content hash
    #     [20:24]   container version
    #     [24:28]   total size
    #     [28:32]   chunk count
    #     [32:..]   uint32 offsets, one per chunk
    # Each chunk header is 8 bytes (4-byte magic + 4-byte size) and
    # is followed by chunk-type-specific payload. The RDEF chunk
    # body lays out as:
    #     [0:4]   constant_buffer_count
    #     [4:8]   constant_buffer_offset
    #     [8:12]  resource_binding_count
    #     [12:16] resource_binding_offset
    #     [16:20] shader_version (program_type | major | minor)
    #     [20:24] compiler_flags
    #     [24:28] creator_offset  (relative to body_start)
    # Offsets in the RDEF body are relative to the START of the
    # chunk body, NOT the start of the file - that's the same
    # convention DXBC uses internally for SGN tables.
    chunk_count_bytes = data[28:32]
    if len(chunk_count_bytes) != 4:
        return None
    chunk_count = struct.unpack("<I", chunk_count_bytes)[0]
    chunk_offsets_bytes = data[32:32 + 4 * chunk_count]
    if len(chunk_offsets_bytes) != 4 * chunk_count:
        return None
    chunk_offsets = struct.unpack(f"<{chunk_count}I", chunk_offsets_bytes)
    for off in chunk_offsets:
        if off + 8 > len(data):
            continue
        magic = data[off:off + 4]
        if magic != b"RDEF":
            continue
        body_start = off + 8
        if body_start + 28 > len(data):
            return None
        creator_offset = struct.unpack("<I", data[body_start + 24:body_start + 28])[0]
        creator_addr = body_start + creator_offset
        # Creator string is null-terminated ASCII.
        end = data.find(b"\0", creator_addr)
        if end == -1:
            return None
        return data[creator_addr:end].decode("ascii", errors="replace")
    return None


def check_microsoft_creator(data, hlsl_path):
    # Reject anything that isn't from Microsoft's fxc.exe. The
    # creator string lives in the RDEF chunk and is what the
    # compiler stamps as its identifier:
    #
    #   Microsoft fxc:     "Microsoft (R) HLSL Shader Compiler 10.1"
    #                      (or similar - version trails the prefix)
    #   Wine vkd3d-shader: "vkd3d-shader 1.10 (Wine bundled)"
    #
    # Microsoft fxc.exe is the only compiler whose DXBC output is
    # known to pass D3D12 PSO validation for the shaders this
    # backend uses. Other compilers may produce bytecode that's
    # technically valid DXBC but trips PSO creation with E_FAIL.
    # cd971cd / da20f5b is the worked example.
    creator = extract_rdef_creator(data)
    if creator is None:
        sys.stderr.write(
            f"error: could not parse RDEF / creator string from output\n"
            f"       for {hlsl_path.name}. The DXBC structure may be\n"
            f"       malformed; refusing to write .inc.\n")
        sys.exit(1)
    if "Microsoft" not in creator:
        sys.stderr.write(
            f"error: {hlsl_path.name} compiled with non-Microsoft fxc.\n"
            f"       Creator string: {creator!r}\n"
            f"       Microsoft's fxc.exe is required - see\n"
            f"       data/shaders/d3d_common/README.md. da20f5b chronicles\n"
            f"       the failure mode when other compilers are used.\n")
        sys.exit(1)


def sanitize_identifier(stem):
    # Map "vram_read.ps" -> "vram_read_ps"; only [A-Za-z0-9_] survive.
    name = re.sub(r"[^A-Za-z0-9_]", "_", stem)
    return name


def compile_one(fxc_cmd, hlsl_path, target, defines=None):
    # fxc /O3 /T <target> /E main /Fo <out> <in> [/D KEY=VALUE]...
    # Output to a temp .dxbc.tmp under INC_DIR so we don't pollute /tmp on
    # restricted-filesystem hosts. Matches the regen_vulkan_spirv.py pattern.
    #
    # Under wine, fxc occasionally exits 0 without writing the output file
    # (wineserver flake under load - the wine32 subsystem is needed by
    # fxc_lite's d3dcompiler_47.dll and can hang briefly). Retry up to 5
    # times before giving up. Each retry is its own subprocess.run so a
    # hung wineserver doesn't pin us forever; the calling shell can
    # CTRL-C out cleanly.
    dxbc_path = INC_DIR / (hlsl_path.stem + ".dxbc.tmp")
    cmd = list(fxc_cmd) + [
        "/T", target,
        "/E", "main",
        "/O3",
        "/nologo",
        "/Fo", str(dxbc_path),
        str(hlsl_path),
    ]
    for d in defines or ():
        cmd.append(f"/D{d}")

    last_stdout = ""
    last_stderr = ""
    for attempt in range(5):
        if dxbc_path.exists():
            dxbc_path.unlink()
        result = subprocess.run(cmd, capture_output=True, text=True)
        last_stdout = result.stdout
        last_stderr = result.stderr
        if result.returncode != 0:
            # Hard fxc error (e.g. HLSL syntax). No point retrying.
            sys.stderr.write(f"fxc failed for {hlsl_path.name}:\n")
            sys.stderr.write(last_stdout)
            sys.stderr.write(last_stderr)
            if dxbc_path.exists():
                dxbc_path.unlink()
            sys.exit(1)
        if dxbc_path.exists():
            break
        # fxc returned 0 but produced no output. Wine flake; retry.
        sys.stderr.write(
            f"warning: fxc returned 0 but no output for {hlsl_path.name} "
            f"(defines={list(defines or ())!r}); retry attempt "
            f"{attempt + 1}/5\n")
    else:
        sys.stderr.write(
            f"error: fxc never produced output for {hlsl_path.name} "
            f"after 5 attempts. Last stdout/stderr follow.\n")
        sys.stderr.write(last_stdout)
        sys.stderr.write(last_stderr)
        sys.exit(1)

    data = dxbc_path.read_bytes()
    dxbc_path.unlink()
    if len(data) < 4:
        sys.stderr.write(
            f"error: {hlsl_path.name} produced {len(data)} bytes of output "
            f"(too short to be DXBC)\n")
        sys.exit(1)
    # DXBC magic: "DXBC" = 0x43425844 little-endian in the first 4 bytes.
    magic = struct.unpack("<I", data[:4])[0]
    if magic != 0x43425844:
        sys.stderr.write(
            f"error: {hlsl_path.name} produced output with bad magic "
            f"(0x{magic:08x}, expected 0x43425844)\n")
        sys.exit(1)
    # Reject non-Microsoft compilers. The check parses the RDEF chunk
    # for the creator string; Wine's bundled d3dcompiler_47.dll
    # (vkd3d-shader) gets refused here.
    check_microsoft_creator(data, hlsl_path)
    return data


def emit_inc(identifier, hlsl_name, target, defines, data, out_path):
    # 16 bytes per line keeps the file diff-friendly without being too tall.
    PER_LINE = 16
    lines = []
    define_summary = " ".join(f"/D{d}" for d in defines) if defines else "(no /D)"
    lines.append(
        f"// Autogenerated from data/shaders/d3d_common/{hlsl_name}.\n"
        f"// DO NOT EDIT. Regenerate with tools/regen_d3d_common_dxbc.py.\n"
        f"// fxc /T {target} /E main /O3 {define_summary}\n"
        f"//\n"
        f"// Included inside namespace D3DCommon::EmbeddedShaders in\n"
        f"// embedded_shaders.cpp. The matching extern declaration lives in\n"
        f"// embedded_shaders.h.\n"
        f"\n"
        f"const uint8_t k_{identifier}[] = {{\n")
    for i in range(0, len(data), PER_LINE):
        chunk = data[i:i + PER_LINE]
        bytes_str = ", ".join(f"0x{b:02x}" for b in chunk)
        lines.append(f"  {bytes_str},\n")
    lines.append(f"}};\n")
    lines.append(f"const size_t k_{identifier}_size_bytes = sizeof(k_{identifier});\n")
    out_path.write_text("".join(lines))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fxc", help="path to fxc.exe (auto-detected if omitted)")
    parser.add_argument("--wine", help="path to wine binary (default: 'wine'); ignored on Windows")
    args = parser.parse_args()

    if not HLSL_DIR.exists():
        sys.stderr.write(f"error: HLSL source directory missing: {HLSL_DIR}\n")
        sys.exit(1)

    INC_DIR.mkdir(parents=True, exist_ok=True)

    # Discover all .hlsl sources. Skip files that don't match a known
    # stage suffix.
    hlsl_files = []
    for entry in sorted(HLSL_DIR.iterdir()):
        if not entry.is_file():
            continue
        for suffix, (target, ident_tag) in STAGE_FROM_SUFFIX.items():
            if entry.name.endswith(suffix):
                stem = entry.name[: -len(suffix)]
                hlsl_files.append((entry, target, ident_tag, stem))
                break
        else:
            if entry.suffix == ".hlsl":
                sys.stderr.write(f"warning: skipping {entry.name} (no known stage suffix)\n")

    if not hlsl_files:
        # Foundation case: no .hlsl files yet, just the README. Print a
        # neutral message and exit zero - the .inc directory is set up,
        # nothing to do until a shader source lands. Don't even look for
        # fxc.exe yet, since the script has no work that needs it.
        print(f"no .hlsl sources under {HLSL_DIR.relative_to(REPO_ROOT)}; nothing to do.")
        return

    fxc_cmd = find_fxc(args.fxc, args.wine)

    total = 0
    for hlsl_path, target, ident_tag, stem in hlsl_files:
        ident_base = sanitize_identifier(stem)
        variants = TEMPLATE_VARIANTS.get(hlsl_path.name)
        if variants is None:
            # No variants table for this file - compile once with no /D
            # defines, emit a single .inc.
            data = compile_one(fxc_cmd, hlsl_path, target)
            identifier = f"{ident_base}_{ident_tag}"
            out_path = INC_DIR / f"{ident_base}_{ident_tag}.inc"
            emit_inc(identifier, hlsl_path.name, target, [], data, out_path)
            total += 1
        else:
            # Template: emit one .inc per (variant_suffix, defines) row.
            for variant_suffix, defines in variants:
                data = compile_one(fxc_cmd, hlsl_path, target, defines)
                identifier = f"{ident_base}_{ident_tag}_{variant_suffix}"
                out_path = INC_DIR / f"{ident_base}_{ident_tag}_{variant_suffix}.inc"
                emit_inc(identifier, hlsl_path.name, target, defines, data, out_path)
                total += 1

    print(f"regenerated {total} .inc file(s) under {INC_DIR.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
