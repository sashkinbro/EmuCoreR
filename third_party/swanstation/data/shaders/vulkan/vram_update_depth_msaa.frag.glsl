// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateVRAMUpdateDepthFragmentShader() when the
// source m_vram_texture is multisampled (sampler2DMS).
//
// See vram_update_depth.frag.glsl for the non-MSAA blob and the full
// rationale. This MSAA twin differs only in:
//   - samp0 is declared sampler2DMS.
//   - The texelFetch sample index is gl_SampleID, which forces the
//     pipeline into per-sample shading per the Vulkan spec (the
//     fragment shader is invoked once per sample of the attachment).
//
// No averaging loop here, unlike the display / VRAM readback MSAA
// blobs - this pipeline writes one depth sample per input sample
// rather than collapsing them into a single fragment.
//
// Bindings (match m_single_sampler_pipeline_layout):
//   set=0 binding=1 - uniform sampler2DMS samp0 (m_vram_texture view
//                     in MSAA mode)

#version 450 core

layout(set = 0, binding = 1) uniform sampler2DMS samp0;

// Unused; kept for interface-match with the screen-quad VS.
layout(location = 0) in VertexData {
  vec2 v_tex0;
};

void main()
{
  gl_FragDepth = texelFetch(samp0, ivec2(gl_FragCoord.xy), gl_SampleID).a;
}
