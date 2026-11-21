// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader() when the
// source m_vram_texture is single-sample (sampler2D).
//
// The "update depth" pipeline rebuilds the depth attachment of the
// upscaled VRAM render pass from the colour attachment's alpha channel
// after a framebuffer recreation (resolution scale change,
// antialiasing toggle, true colour toggle). Single sampler bound at
// set=0 binding=1, no UBO, no push constants - the shader is literally
// "copy alpha to depth".
//
// Two blobs handle the structural MSAA split (sampler2D vs sampler2DMS,
// matching the display / VRAM readback pattern). No spec constants
// needed on the non-MSAA blob - there is nothing to specialise.
//
// Bindings (match m_single_sampler_pipeline_layout):
//   set=0 binding=1 - uniform sampler2D samp0 (m_vram_texture view)
//
// The pipeline layout declares push constants for ABI symmetry with
// other m_single_sampler_pipeline_layout consumers; the shader does
// not read any, and the C++ caller (UpdateDepthBufferFromMaskBit)
// does not push any either.

#version 450 core

layout(set = 0, binding = 1) uniform sampler2D samp0;

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

void main()
{
  gl_FragDepth = texelFetch(samp0, ivec2(gl_FragCoord.xy), 0).a;
}
