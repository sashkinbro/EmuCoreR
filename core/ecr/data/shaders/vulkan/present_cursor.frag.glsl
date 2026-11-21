// Pre-baked Vulkan source for the presentation-stage cursor FS.
//
// Previously lived as an inline R"()" string literal in
// LibretroVulkanHostDisplay::CreateResources() in src/core/gpu_hw_vulkan.cpp.
// Differs from present_display.frag.glsl in that it preserves the source
// alpha so the software cursor can be alpha-blended over the framebuffer.

#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(location = 0) in vec2 v_tex0;
layout(location = 0) out vec4 o_col0;

void main()
{
  o_col0 = texture(samp0, v_tex0);
}
