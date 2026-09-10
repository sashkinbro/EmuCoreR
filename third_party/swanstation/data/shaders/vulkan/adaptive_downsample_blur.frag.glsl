// Pre-baked Vulkan source for
// GPU_HW_ShaderGen::GenerateAdaptiveDownsampleBlurFragmentShader().
//
// Vulkan-only port. Originally "mipmap_blur.glsl" from parallel-rsx. Reads
// a single-channel weight texture, applies a 3x3 separable-ish kernel, and
// writes out a smoothed weight. The output is used by the composite pass
// to choose mip level per output pixel.
//
// No specialization constants - the shader body references none of the
// WriteCommonFunctions constants (RESOLUTION_SCALE / MULTISAMPLES /
// PER_SAMPLE_SHADING). u_rcp_resolution arrives via push constants.

#version 450 core

layout(set = 0, binding = 0) uniform sampler2D samp0;

layout(push_constant) uniform PushConstants {
  vec2 u_uv_min;
  vec2 u_uv_max;
  vec2 u_rcp_resolution;
  float sample_level;
};

layout(location = 0) in VertexData {
  vec2 v_tex0;
};

layout(location = 0) out vec4 o_col0;

#define UV(x, y) clamp((v_tex0 + vec2(x, y) * u_rcp_resolution), u_uv_min, u_uv_max)

void main()
{
  float bias = 0.0;
  const float w0 = 0.25;
  const float w1 = 0.125;
  const float w2 = 0.0625;
  bias += w2 * texture(samp0, UV(-1.0, -1.0)).a;
  bias += w2 * texture(samp0, UV(+1.0, -1.0)).a;
  bias += w2 * texture(samp0, UV(-1.0, +1.0)).a;
  bias += w2 * texture(samp0, UV(+1.0, +1.0)).a;
  bias += w1 * texture(samp0, UV( 0.0, -1.0)).a;
  bias += w1 * texture(samp0, UV(-1.0,  0.0)).a;
  bias += w1 * texture(samp0, UV(+1.0,  0.0)).a;
  bias += w1 * texture(samp0, UV( 0.0, +1.0)).a;
  bias += w0 * texture(samp0, UV( 0.0,  0.0)).a;
  o_col0 = vec4(bias, bias, bias, bias);
}
