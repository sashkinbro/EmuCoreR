// Pre-baked Vulkan source for the presentation-stage display FS.
//
// Previously lived as an inline R"()" string literal in
// LibretroVulkanHostDisplay::CreateResources() in src/core/gpu_hw_vulkan.cpp.
// Samples the emulator's rendered frame (a sampler2D bound at descriptor
// set 0, binding 0) and writes it opaque to the libretro framebuffer.

#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(location = 0) in vec2 v_tex0;
layout(location = 0) out vec4 o_col0;

void main()
{
  o_col0 = vec4(texture(samp0, v_tex0).rgb, 1.0);
}
