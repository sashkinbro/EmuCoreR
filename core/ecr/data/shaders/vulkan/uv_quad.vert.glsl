// Pre-baked Vulkan source for ShaderGen::GenerateUVQuadVertexShader().
//
// Vulkan-only port of the runtime ShaderGen output. The push-constant block
// layout (two vec2s, 16 bytes total) is fixed by the existing pipeline
// layout and the values supplied by callers that bind UV-quad pipelines;
// changing it would require coordinated C++ changes in gpu_hw_vulkan.cpp.

#version 450 core

layout(push_constant) uniform PushConstants {
  vec2 u_uv_min;
  vec2 u_uv_max;
};

layout(location = 0) out VertexData {
  vec2 v_tex0;
};

void main()
{
  v_tex0 = vec2(float((uint(gl_VertexIndex) << 1) & 2u), float(uint(gl_VertexIndex) & 2u));
  gl_Position = vec4(v_tex0 * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 0.0, 1.0);
  v_tex0 = u_uv_min + (u_uv_max - u_uv_min) * v_tex0;
  // Flip Y to match the convention of ShaderGen output for Vulkan.
  gl_Position.y = -gl_Position.y;
}
