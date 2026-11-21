// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateAdaptiveDownsampleCompositeFragmentShader().
//
// Vulkan-only port. Originally "mipmap_resolve.glsl" from parallel-rsx.
// Reads the per-pixel mip-bias (single-channel) computed by the blur pass
// from samp1, picks a mip level on samp0 (the original color pyramid)
// scaled by (RESOLUTION_SCALE - 1), and writes the resolved color.
//
// Spec constant: RESOLUTION_SCALE at constant_id=0 per the project-wide
// convention (see Vulkan::SpecConstants header comment in builders.h).
// The PSX VRAM size is fixed at 1024x512; the *upscaled* VRAM size is
// VRAM_SIZE = (1024, 512) * RESOLUTION_SCALE, derived inside main() since
// GLSL forbids spec-const-derived expressions at global 'const' scope.
// The driver folds these to literals at vkCreateGraphicsPipelines time
// once the spec constant value is known.
//
// Descriptor binding offsets match ShaderGen::DeclareTexture for Vulkan:
// texture index N -> layout binding (N + 1). The pipeline-layout side
// (m_downsample_composite_descriptor_set_layout) declares bindings 1 and 2.

#version 450 core

layout(constant_id = 0) const uint RESOLUTION_SCALE = 1u;

layout(set = 0, binding = 1) uniform sampler2D samp0;
layout(set = 0, binding = 2) uniform sampler2D samp1;

layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

void main()
{
  // RCP_VRAM_SIZE = 1.0 / float2(VRAM_WIDTH, VRAM_HEIGHT) / RESOLUTION_SCALE
  vec2 rcp_vram_size = vec2(1.0 / 1024.0, 1.0 / 512.0) / float(RESOLUTION_SCALE);
  vec2 uv = gl_FragCoord.xy * rcp_vram_size;
  float bias = texture(samp1, uv).r;
  float mip = float(RESOLUTION_SCALE - 1u) * bias;
  vec3 color = textureLod(samp0, uv, mip).rgb;
  o_col0 = vec4(color, 1.0);
}
