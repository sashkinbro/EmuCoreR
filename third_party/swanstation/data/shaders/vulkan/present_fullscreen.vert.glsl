// Pre-baked Vulkan source for the presentation-stage fullscreen-quad VS.
//
// Previously lived as an inline R"()" string literal in
// LibretroVulkanHostDisplay::CreateResources() in src/core/gpu_hw_vulkan.cpp.
// It is distinct from the GPU_HW screen-quad VS - this one has a u_src_rect
// push constant that selects a sub-rectangle of the source texture, used to
// present the emulator's rendered frame (and the software cursor) into the
// libretro framebuffer.
//
// The push-constant struct ('PushConstants', defined C++-side as a vec4)
// must remain byte-compatible with the binding emitted by the pipeline
// layout in LibretroVulkanHostDisplay.

#version 450 core

layout(push_constant) uniform PushConstants {
  vec4 u_src_rect;
};

layout(location = 0) out vec2 v_tex0;

void main()
{
  vec2 pos = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  v_tex0 = u_src_rect.xy + pos * u_src_rect.zw;
  gl_Position = vec4(pos * vec2(2.0f, -2.0f) + vec2(-1.0f, 1.0f), 0.0f, 1.0f);
  gl_Position.y = -gl_Position.y;
}
